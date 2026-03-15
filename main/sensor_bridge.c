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
#include "ld2450.h"
#include "nvs_config.h"
#include "sensor_bridge.h"
#include "zigbee_defs.h"
#include "zigbee_signal_handlers.h"
#include "crash_diag.h"

static const char *TAG = "sensor_bridge";

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

static void sensor_poll_cb(uint8_t param)
{
    (void)param;
    esp_zb_scheduler_alarm(sensor_poll_cb, ALARM_PARAM_POLL, SENSOR_POLL_INTERVAL_MS);

    if (!zigbee_is_network_joined()) return;

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
    uint32_t main_cooldown_ticks = pdMS_TO_TICKS(cfg.occupancy_cooldown_sec[0] * 1000);
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
        }
    }

    /* EPs 2-11: Per-zone occupancy */
    for (int i = 0; i < 10; i++) {
        bool zone_occ = state.zone_occupied[i];
        uint32_t zone_cooldown_ticks = pdMS_TO_TICKS(cfg.occupancy_cooldown_sec[i + 1] * 1000);
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

static void configure_reporting_for_occ(uint8_t ep)
{
    esp_zb_zcl_reporting_info_t rpt = {0};
    rpt.direction   = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    rpt.ep          = ep;
    rpt.cluster_id  = ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING;
    rpt.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    rpt.attr_id     = ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID;
    rpt.u.send_info.min_interval     = REPORT_MIN_INTERVAL;
    rpt.u.send_info.max_interval     = REPORT_MAX_INTERVAL;
    rpt.u.send_info.def_min_interval = REPORT_MIN_INTERVAL;
    rpt.u.send_info.def_max_interval = REPORT_MAX_INTERVAL;
    rpt.u.send_info.delta.u8         = 0;
    rpt.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    rpt.manuf_code     = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    esp_zb_zcl_update_reporting_info(&rpt);
}

static void configure_all_reporting(void)
{
    /* Occupancy reporting on all 6 endpoints */
    configure_reporting_for_occ(ZB_EP_MAIN);
    for (int i = 0; i < ZB_EP_ZONE_COUNT; i++) {
        configure_reporting_for_occ(ZB_EP_ZONE(i));
    }
    /* Boot stats: 5-min keepalive guarantees Z2M gets fresh values after any rejoin */
    configure_reporting_for_diag_attr(ZB_ATTR_BOOT_COUNT,      REPORT_MAX_INTERVAL);
    configure_reporting_for_diag_attr(ZB_ATTR_RESET_REASON,    REPORT_MAX_INTERVAL);
    configure_reporting_for_diag_attr(ZB_ATTR_LAST_UPTIME_SEC, REPORT_MAX_INTERVAL);
    /* Min free heap: no keepalive, reported only alongside occupancy/sensor changes */
    configure_reporting_for_diag_attr(ZB_ATTR_MIN_FREE_HEAP,   0);
    ESP_LOGI(TAG, "Reporting configured for all endpoints");
}

void sensor_bridge_start(void)
{
    ESP_LOGI(TAG, "Starting sensor bridge (poll every %d ms)", SENSOR_POLL_INTERVAL_MS);
    configure_all_reporting();
    esp_zb_scheduler_alarm(sensor_poll_cb, ALARM_PARAM_POLL, SENSOR_POLL_INTERVAL_MS);
}
