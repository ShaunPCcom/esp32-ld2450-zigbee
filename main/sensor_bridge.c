// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee_core.h"

/* Project */
#include "coordinator_fallback.h"
#include "ld2450.h"
#include "ld2450_zone_csv.h"
#include "nvs_config.h"
#include "sensor_bridge.h"
#include "zigbee_defs.h"
#include "zigbee_signal_handlers.h"
#include "crash_diag.h"

static const char *TAG = "sensor_bridge";

/* Set by sensor_bridge_mark_config_dirty(); cleared after push_config_attrs() */
static volatile bool s_config_dirty = false;

/* Sensor poll interval (ms) - LD2450 outputs at 10Hz (100ms) */
#define SENSOR_POLL_INTERVAL_MS  100

/* Reporting intervals (seconds) */
#define REPORT_MIN_INTERVAL   0
#define REPORT_MAX_INTERVAL   300

/* Scheduler alarm param */
#define ALARM_PARAM_POLL    0

/* ---- State tracking for change detection ---- */
static bool s_last_occupied = false;
static bool s_last_zone_occ[10] = {false};
static uint8_t s_last_target_count = 0;
static char s_last_coords[64] = {0};

/* ---- Cooldown tracking (per endpoint: 0=main, 1-10=zones) ---- */
static uint32_t s_last_report_time[11] = {0};
static bool s_pending_clear[11] = {false};      /* tracking pending Clear reports */
static uint32_t s_clear_start_time[11] = {0};  /* when Clear was first detected */

/* ---- Occupancy delay tracking (per endpoint: 0=main, 1-10=zones) ---- */
static bool s_pending_occupied[11] = {false};      /* tracking pending Occupied reports */
static int64_t s_occupied_start_time[11] = {0};   /* when Occupied was first detected (microseconds) */
static uint32_t s_last_min_free_heap = 0;

/* ---- Raw state tracking (transition detection, independent of reported state) ----
 *
 * s_last_occupied / s_last_zone_occ track the last *reported* state and must
 * only be updated when an attribute report is actually sent.  Using them for
 * transition detection causes the delay-cancellation path to be skipped when
 * occupancy clears during the delay window (before a report fires), because
 * "current_clear != last_reported_clear" evaluates false.  This leaves
 * s_pending_occupied stuck true with a stale timestamp, causing the next real
 * detection to fire immediately (no delay) — observed as Zone 1 reporting
 * before Zone 2 and general light-response sluggishness across rooms.
 *
 * s_raw_occupied / s_raw_zone_occ track the actual sensor state every poll
 * cycle and are the correct variables for edge-detection.
 */
static bool s_raw_occupied = false;
static bool s_raw_zone_occ[10] = {false};

/* ================================================================== */
/*  Sensor bridge: poll LD2450 and update Zigbee attributes            */
/* ================================================================== */

static void format_coords_string(const ld2450_state_t *state, char *buf, size_t buf_size)
{
    /* Format: "x1,y1;x2,y2;x3,y3" with ZCL char-string length prefix */
    char tmp[48];
    int pos = 0;

    for (int i = 0; i < 3; i++) {
        if (state->targets[i].present) {
            if (pos > 0) {
                pos += snprintf(tmp + pos, sizeof(tmp) - pos, ";");
            }
            pos += snprintf(tmp + pos, sizeof(tmp) - pos, "%d,%d",
                          (int)state->targets[i].x_mm, (int)state->targets[i].y_mm);
        }
    }

    if (pos == 0) {
        buf[0] = 0;  /* ZCL empty string */
        buf[1] = 0;
    } else {
        buf[0] = (char)pos;  /* ZCL length prefix */
        memcpy(buf + 1, tmp, pos);
        buf[pos + 1] = 0;
    }
}

/* Push all writable config attributes into the ZCL attribute table.
 * Called from sensor_poll_cb (Zigbee task context) when s_config_dirty is set. */
static void push_config_attrs(void)
{
    nvs_config_t cfg;
    nvs_config_get(&cfg);

#define SET_ATTR(ep, cluster, attr, val) \
    esp_zb_zcl_set_attribute_val((ep), (cluster), \
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, (attr), (val), false)

    /* ---- Sensor config ---- */
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_MAX_DISTANCE,      &cfg.max_distance_mm);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_ANGLE_LEFT,        &cfg.angle_left_deg);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_ANGLE_RIGHT,       &cfg.angle_right_deg);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_TRACKING_MODE,     &cfg.tracking_mode);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_COORD_PUBLISHING,  &cfg.publish_coords);

    /* ---- Main EP occupancy timing ---- */
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_OCCUPANCY_COOLDOWN, &cfg.occupancy_cooldown_sec[0]);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_OCCUPANCY_DELAY,    &cfg.occupancy_delay_ms[0]);

    /* ---- Coordinator fallback ---- */
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_FALLBACK_MODE,      &cfg.fallback_mode);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_FALLBACK_ENABLE,    &cfg.fallback_enable);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_FALLBACK_COOLDOWN,  &cfg.fallback_cooldown_sec[0]);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_HARD_TIMEOUT_SEC,   &cfg.hard_timeout_sec);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_ACK_TIMEOUT_MS,     &cfg.ack_timeout_ms);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_HEARTBEAT_ENABLE,   &cfg.heartbeat_enable);
    SET_ATTR(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG, ZB_ATTR_HEARTBEAT_INTERVAL, &cfg.heartbeat_interval_sec);

    /* ---- Zone config (each zone on its own EP) ---- */
    /* With each zone on its own cluster instance, ZBoss handles CHAR_STRING reports
     * independently per zone — no more "only first zone fires" reporting bug. */
    char zb_str[ZB_ZONE_COORDS_MAX_LEN + 2];
    char csv[ZB_ZONE_COORDS_MAX_LEN];

    for (int n = 0; n < ZB_EP_ZONE_COUNT; n++) {
        uint8_t ep = ZB_EP_ZONE(n);
        SET_ATTR(ep, ZB_CLUSTER_LD2450_CONFIG,
                 ZB_ATTR_ZONE_VERTEX_COUNT(n), &cfg.zones[n].vertex_count);

        SET_ATTR(ep, ZB_CLUSTER_LD2450_CONFIG,
                 ZB_ATTR_ZONE_COOLDOWN(n), &cfg.occupancy_cooldown_sec[n + 1]);

        SET_ATTR(ep, ZB_CLUSTER_LD2450_CONFIG,
                 ZB_ATTR_ZONE_DELAY(n), &cfg.occupancy_delay_ms[n + 1]);

        /* Coords: ZCL CHAR_STRING = length byte + CSV payload.
         * false flag: ZBoss copies value into its own attr storage — local buffer safe. */
        zone_to_csv(&cfg.zones[n], csv, sizeof(csv));
        size_t len = strlen(csv);
        zb_str[0] = (char)len;
        memcpy(zb_str + 1, csv, len + 1);
        SET_ATTR(ep, ZB_CLUSTER_LD2450_CONFIG,
                 ZB_ATTR_ZONE_COORDS(n), zb_str);
    }

#undef SET_ATTR

    ESP_LOGD(TAG, "Config attrs pushed to ZCL table");
}

void sensor_bridge_mark_config_dirty(void)
{
    s_config_dirty = true;
}

static void sensor_poll_cb(uint8_t param)
{
    (void)param;
    esp_zb_scheduler_alarm(sensor_poll_cb, ALARM_PARAM_POLL, SENSOR_POLL_INTERVAL_MS);

    if (!zigbee_is_network_joined()) return;

    /* Push all config attrs when dirtied by an external source (e.g. web UI) */
    if (s_config_dirty) {
        s_config_dirty = false;
        push_config_attrs();
    }

    /* Update RTC uptime every poll — pure memory write, no Zigbee traffic */
    crash_diag_update_uptime((uint32_t)(esp_timer_get_time() / 1000000ULL));

    ld2450_state_t state;
    if (ld2450_get_state(&state) != ESP_OK) return;

    ld2450_runtime_cfg_t rt_cfg;
    ld2450_get_runtime_cfg(&rt_cfg);

    /* Get current config and time */
    nvs_config_t cfg;
    nvs_config_get(&cfg);
    uint32_t current_ticks = xTaskGetTickCount();
    int64_t current_time_us = esp_timer_get_time();
    bool any_sensor_change = false;

    /* EP 1: Overall occupancy */
    bool occupied = state.occupied_global;
    uint32_t main_cooldown_sec = cfg.occupancy_cooldown_sec[0];
    uint32_t main_cooldown_ticks = pdMS_TO_TICKS(main_cooldown_sec * 1000);
    int64_t main_delay_us = cfg.occupancy_delay_ms[0] * 1000LL;

    bool raw_was_occupied = s_raw_occupied;
    s_raw_occupied = occupied;
    if (occupied != raw_was_occupied) {
        if (!occupied) {
            /* State went Occupied → Clear: Cancel pending occupied, start cooldown */
            s_pending_occupied[0] = false;
            if (!s_pending_clear[0]) {
                s_pending_clear[0] = true;
                s_clear_start_time[0] = current_ticks;
            }
        } else {
            /* State went Clear → Occupied: Cancel pending clear, start delay timer */
            s_pending_clear[0] = false;
            if (!s_pending_occupied[0]) {
                s_pending_occupied[0] = true;
                s_occupied_start_time[0] = current_time_us;
            }
        }
    }

    /* Check for pending Occupied report that has completed delay */
    if (s_pending_occupied[0] && occupied) {
        if (main_delay_us == 0 || (current_time_us - s_occupied_start_time[0]) >= main_delay_us) {
            /* Delay complete and still occupied - report it */
            uint8_t val = 1;
            esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
                ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                &val, false);
            s_last_occupied = true;
            s_last_report_time[0] = current_ticks;
            s_pending_occupied[0] = false;
            any_sensor_change = true;
            coordinator_fallback_on_occupancy_change(ZB_EP_MAIN, true);
            coordinator_fallback_report_occupancy(ZB_EP_MAIN, true);
        }
    }

    /* Check for pending Clear report that has completed cooldown */
    if (s_pending_clear[0] && !occupied) {
        if (main_cooldown_ticks == 0 || (current_ticks - s_clear_start_time[0]) >= main_cooldown_ticks) {
            /* Cooldown complete and still clear - report it */
            uint8_t val = 0;
            esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
                ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                &val, false);
            s_last_occupied = false;
            s_last_report_time[0] = current_ticks;
            s_pending_clear[0] = false;
            any_sensor_change = true;
            coordinator_fallback_on_occupancy_change(ZB_EP_MAIN, false);
            coordinator_fallback_report_occupancy(ZB_EP_MAIN, false);
        }
    }

    /* EPs 2-11: Per-zone occupancy */
    for (int i = 0; i < 10; i++) {
        bool zone_occ = state.zone_occupied[i];
        uint32_t zone_cooldown_sec = cfg.occupancy_cooldown_sec[i + 1];
        uint32_t zone_cooldown_ticks = pdMS_TO_TICKS(zone_cooldown_sec * 1000);
        int64_t zone_delay_us = cfg.occupancy_delay_ms[i + 1] * 1000LL;

        bool raw_was = s_raw_zone_occ[i];
        s_raw_zone_occ[i] = zone_occ;
        if (zone_occ != raw_was) {
            if (!zone_occ) {
                /* State went Occupied → Clear: Cancel pending occupied, start cooldown */
                s_pending_occupied[i + 1] = false;
                if (!s_pending_clear[i + 1]) {
                    s_pending_clear[i + 1] = true;
                    s_clear_start_time[i + 1] = current_ticks;
                }
            } else {
                /* State went Clear → Occupied: Cancel pending clear, start delay timer */
                s_pending_clear[i + 1] = false;
                if (!s_pending_occupied[i + 1]) {
                    s_pending_occupied[i + 1] = true;
                    s_occupied_start_time[i + 1] = current_time_us;
                }
            }
        }

        /* Check for pending Occupied report that has completed delay */
        if (s_pending_occupied[i + 1] && zone_occ) {
            if (zone_delay_us == 0 || (current_time_us - s_occupied_start_time[i + 1]) >= zone_delay_us) {
                /* Delay complete and still occupied - report it */
                uint8_t val = 1;
                esp_zb_zcl_set_attribute_val(ZB_EP_ZONE(i),
                    ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                    ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                    &val, false);
                s_last_zone_occ[i] = true;
                s_last_report_time[i + 1] = current_ticks;
                s_pending_occupied[i + 1] = false;
                any_sensor_change = true;
                coordinator_fallback_on_occupancy_change(ZB_EP_ZONE(i), true);
                coordinator_fallback_report_occupancy(ZB_EP_ZONE(i), true);
            }
        }

        /* Check for pending Clear report that has completed cooldown */
        if (s_pending_clear[i + 1] && !zone_occ) {
            if (zone_cooldown_ticks == 0 || (current_ticks - s_clear_start_time[i + 1]) >= zone_cooldown_ticks) {
                /* Cooldown complete and still clear - report it */
                uint8_t val = 0;
                esp_zb_zcl_set_attribute_val(ZB_EP_ZONE(i),
                    ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                    ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                    &val, false);
                s_last_zone_occ[i] = false;
                s_last_report_time[i + 1] = current_ticks;
                s_pending_clear[i + 1] = false;
                any_sensor_change = true;
                coordinator_fallback_on_occupancy_change(ZB_EP_ZONE(i), false);
                coordinator_fallback_report_occupancy(ZB_EP_ZONE(i), false);
            }
        }
    }

    /* EP 1: Target count */
    uint8_t count = state.target_count_effective;
    if (count != s_last_target_count) {
        esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
            ZB_CLUSTER_LD2450_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ZB_ATTR_TARGET_COUNT,
            &count, false);
        s_last_target_count = count;
        any_sensor_change = true;
    }

    /* EP 1: Target coordinates (only if publishing enabled) */
    if (rt_cfg.publish_coords) {
        char coords[64];
        format_coords_string(&state, coords, sizeof(coords));
        if (memcmp(coords, s_last_coords, sizeof(coords)) != 0) {
            esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
                ZB_CLUSTER_LD2450_CONFIG,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ZB_ATTR_TARGET_COORDS,
                coords, false);
            memcpy(s_last_coords, coords, sizeof(s_last_coords));
            any_sensor_change = true;
        }
    }

    /* Update min_free_heap only when another sensor value changed this poll cycle */
    if (any_sensor_change) {
        uint32_t heap = esp_get_minimum_free_heap_size();
        if (heap != s_last_min_free_heap) {
            esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
                ZB_CLUSTER_LD2450_CONFIG,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ZB_ATTR_MIN_FREE_HEAP,
                &heap, false);
            s_last_min_free_heap = heap;
        }
    }
}

static void configure_reporting_for_diag_attr(uint16_t attr_id, uint16_t max_interval)
{
    esp_zb_zcl_reporting_info_t rpt = {0};
    rpt.direction    = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    rpt.ep           = ZB_EP_MAIN;
    rpt.cluster_id   = ZB_CLUSTER_LD2450_CONFIG;
    rpt.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    rpt.attr_id      = attr_id;
    rpt.u.send_info.min_interval     = REPORT_MIN_INTERVAL;
    rpt.u.send_info.max_interval     = max_interval;
    rpt.u.send_info.def_min_interval = REPORT_MIN_INTERVAL;
    rpt.u.send_info.def_max_interval = max_interval;
    rpt.u.send_info.delta.u32        = 0;
    rpt.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    rpt.manuf_code     = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    esp_zb_zcl_update_reporting_info(&rpt);
}

static void configure_all_reporting(void)
{
    /* Occupancy reporting is handled by coordinator_fallback_report_occupancy()
     * with explicit ACK tracking and retry.  No auto-report entries here. */

    /* Boot stats: 5-min keepalive guarantees Z2M gets fresh values after any rejoin */
    configure_reporting_for_diag_attr(ZB_ATTR_BOOT_COUNT,      REPORT_MAX_INTERVAL);
    configure_reporting_for_diag_attr(ZB_ATTR_RESET_REASON,    REPORT_MAX_INTERVAL);
    configure_reporting_for_diag_attr(ZB_ATTR_LAST_UPTIME_SEC, REPORT_MAX_INTERVAL);
    /* Min free heap: no keepalive, reported only alongside occupancy/sensor changes */
    configure_reporting_for_diag_attr(ZB_ATTR_MIN_FREE_HEAP,   0);
    /* Soft fault: report on any change (delta=0) */
    configure_reporting_for_diag_attr(ZB_ATTR_SOFT_FAULT,      0);

    /* Zone config attrs: no device-side entries needed.
     * Each zone EP has its own cluster instance with only 4 attrs, so Z2M's
     * configureReporting entries work correctly without device-side overrides.
     * Device-side entries would double the reporting table size and exceed
     * ZBoss's default allocation. */

    ESP_LOGI(TAG, "Reporting configured for all endpoints");
}

void sensor_bridge_start(void)
{
    static bool s_started = false;
    if (s_started) {
        ESP_LOGW(TAG, "sensor_bridge_start() called again — ignoring duplicate");
        return;
    }
    s_started = true;

    ESP_LOGI(TAG, "Starting sensor bridge (poll every %d ms)", SENSOR_POLL_INTERVAL_MS);
    configure_all_reporting();
    coordinator_fallback_start_keepalive();
    /* Push real config values on first poll — corrects the max-length padded
     * placeholder used to pre-allocate ZBoss's internal CHAR_STRING buffers. */
    s_config_dirty = true;
    esp_zb_scheduler_alarm(sensor_poll_cb, ALARM_PARAM_POLL, SENSOR_POLL_INTERVAL_MS);
}
