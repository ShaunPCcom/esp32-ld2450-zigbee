// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Zigbee SDK */
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_common.h"

/* Project */
#include "board_config.h"
#include "board_led.h"
#include "coordinator_fallback.h"
#include "crash_diag.h"
#include "ld2450_zone_csv.h"
#include "nvs_config.h"
#include "version.h"
#include "zigbee_attr_handler.h"
#include "zigbee_defs.h"
#include "zigbee_init.h"
#include "zigbee_ota.h"

static const char *TAG = "zigbee_init";

/* ================================================================== */
/*  Helper: create cluster lists for endpoints                         */
/* ================================================================== */

static esp_zb_cluster_list_t *create_main_ep_clusters(void)
{
    /* Basic cluster */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ZB_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ZB_MODEL_IDENTIFIER);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, (void *)ZB_SW_BUILD_ID);

    /* Identify cluster */
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *identify = esp_zb_identify_cluster_create(&identify_cfg);

    /* Occupancy Sensing cluster */
    esp_zb_occupancy_sensing_cluster_cfg_t occ_cfg = {
        .occupancy         = 0,
        .sensor_type       = ESP_ZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_RESERVED,
        .sensor_type_bitmap = (1 << 2),
    };
    esp_zb_attribute_list_t *occ = esp_zb_occupancy_sensing_cluster_create(&occ_cfg);

    /* Custom cluster 0xFC00 - LD2450 config + sensor data */
    esp_zb_attribute_list_t *custom = esp_zb_zcl_attr_list_create(ZB_CLUSTER_LD2450_CONFIG);

    /* Load current config for initial values */
    nvs_config_t cfg;
    nvs_config_get(&cfg);
    uint8_t init_fallback_mode = cfg.fallback_mode;

    uint8_t zero_u8 = 0;
    uint16_t init_dist = cfg.max_distance_mm;
    uint8_t init_al = cfg.angle_left_deg;
    uint8_t init_ar = cfg.angle_right_deg;
    uint8_t init_mode = cfg.tracking_mode;
    uint8_t init_coords = cfg.publish_coords;
    uint16_t init_cooldown = cfg.occupancy_cooldown_sec[0];
    uint16_t init_delay = cfg.occupancy_delay_ms[0];

    /* ZCL char-string: first byte = length, rest = chars. Empty string = "\x00" */
    char empty_str[2] = {0x00, 0x00};

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_TARGET_COUNT,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &zero_u8);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_TARGET_COORDS,
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        empty_str);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_MAX_DISTANCE,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_dist);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_ANGLE_LEFT,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_al);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_ANGLE_RIGHT,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_ar);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_TRACKING_MODE,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_mode);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_COORD_PUBLISHING,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_coords);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_OCCUPANCY_COOLDOWN,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_cooldown);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_OCCUPANCY_DELAY,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_delay);

    /* Crash diagnostics (read-only attributes for remote debugging) */
    crash_diag_data_t diag;
    crash_diag_get_data(&diag);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_BOOT_COUNT,
        ESP_ZB_ZCL_ATTR_TYPE_U32,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &diag.boot_count);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_RESET_REASON,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &diag.reset_reason);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_LAST_UPTIME_SEC,
        ESP_ZB_ZCL_ATTR_TYPE_U32,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &diag.last_uptime_sec);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_MIN_FREE_HEAP,
        ESP_ZB_ZCL_ATTR_TYPE_U32,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &diag.min_free_heap);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_RESTART,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY,
        &zero_u8);

    static uint8_t s_factory_reset_attr = 0;
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_FACTORY_RESET,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY,
        &s_factory_reset_attr);

    /* Fallback mode attribute (0x0024) — RW + reportable so coordinator sees transitions */
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_FALLBACK_MODE,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_fallback_mode);

    /* Heartbeat watchdog attributes (0x0026-0x0028) */
    static uint8_t  s_hb_enable   = 0;
    static uint16_t s_hb_interval = 120;
    static uint8_t  s_hb_write    = 0;
    {
        nvs_config_t hb_cfg;
        nvs_config_get(&hb_cfg);
        s_hb_enable   = hb_cfg.heartbeat_enable;
        s_hb_interval = hb_cfg.heartbeat_interval_sec;
    }
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_HEARTBEAT_ENABLE,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_hb_enable);
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_HEARTBEAT_INTERVAL,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_hb_interval);
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_HEARTBEAT,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY,
        &s_hb_write);

    /* Soft/hard two-tier fallback attributes (0x0029-0x002C) */
    static uint8_t  s_fb_enable     = 0;
    static uint8_t  s_fb_zero_u8    = 0;   /* soft_fault initial value = 0 */
    static uint8_t  s_hard_to_sec   = 10;
    static uint16_t s_ack_to_ms     = 2000;
    {
        nvs_config_t tier_cfg;
        nvs_config_get(&tier_cfg);
        s_fb_enable   = tier_cfg.fallback_enable;
        s_hard_to_sec = tier_cfg.hard_timeout_sec;
        if (s_hard_to_sec == 0) s_hard_to_sec = 10;
        s_ack_to_ms   = tier_cfg.ack_timeout_ms;
        if (s_ack_to_ms == 0) s_ack_to_ms = 2000;
    }
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_FALLBACK_ENABLE,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_fb_enable);
    /* soft_fault: R+Report, firmware-only write; HA observes changes */
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_SOFT_FAULT,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_fb_zero_u8);
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_HARD_TIMEOUT_SEC,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_hard_to_sec);
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_ACK_TIMEOUT_MS,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_ack_to_ms);

    /* Fallback cooldown attributes (0x0025 = main, 0x0070-0x0079 = zones) */
    static uint16_t s_fb_cool_main = 300;
    static uint16_t s_fb_cool_zone[10] = {300, 300, 300, 300, 300, 300, 300, 300, 300, 300};
    {
        nvs_config_t fb_cfg;
        nvs_config_get(&fb_cfg);
        s_fb_cool_main = fb_cfg.fallback_cooldown_sec[0];
        for (int n = 0; n < 10; n++) {
            s_fb_cool_zone[n] = fb_cfg.fallback_cooldown_sec[n + 1];
        }
    }
    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_FALLBACK_COOLDOWN,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_fb_cool_main);
    for (int n = 0; n < 10; n++) {
        esp_zb_custom_cluster_add_custom_attr(custom,
            ZB_ATTR_FALLBACK_ZONE_COOL_BASE + n,
            ESP_ZB_ZCL_ATTR_TYPE_U16,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &s_fb_cool_zone[n]);
    }

    /* Assemble cluster list */
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cl, identify, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_occupancy_sensing_cluster(cl, occ, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, custom, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* On/Off CLIENT cluster — enables binding-based dispatch for fallback mode.
     * Client On/Off (0x0006) and server Occupancy Sensing (0x0406) are different
     * cluster IDs on the same endpoint, which is valid ZCL. */
    esp_zb_attribute_list_t *on_off_client = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(cl, on_off_client, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));

    /* Add OTA cluster */
    zigbee_ota_config_t ota_cfg = ZIGBEE_OTA_CONFIG_DEFAULT();
    ota_cfg.manufacturer_code = 0x131B;  /* Espressif */
#if CONFIG_IDF_TARGET_ESP32C6
    ota_cfg.image_type = 0x0003;         /* LD2450 C6 application */
#else
    ota_cfg.image_type = 0x0001;         /* LD2450 H2 application */
#endif
    ota_cfg.current_file_version = FIRMWARE_VERSION;  /* Derived from version.h */
    ota_cfg.hw_version = 1;
    ota_cfg.query_interval_minutes = 1440;  /* Check every 24 hours */
    ESP_ERROR_CHECK(zigbee_ota_init(cl, ZB_EP_MAIN, &ota_cfg));

    return cl;
}

/* Zone EPs (2-11): occupancy sensing + per-zone config cluster 0xFC00.
 * Each zone EP carries exactly 4 attrs for that zone (vertex_count, coords, cooldown, delay).
 * Keeping each zone in its own cluster instance avoids the ZBoss CHAR_STRING reporting bug
 * where only the first CHAR_STRING attr in a cluster fires independent reports. */
static esp_zb_cluster_list_t *create_zone_ep_clusters(int zone_idx)
{
    /* Basic cluster (required for Z2M re-interview — must carry mfr/model/sw_build_id) */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ZB_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ZB_MODEL_IDENTIFIER);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, (void *)ZB_SW_BUILD_ID);

    /* Identify cluster */
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *identify = esp_zb_identify_cluster_create(&identify_cfg);

    /* Occupancy Sensing cluster */
    esp_zb_occupancy_sensing_cluster_cfg_t occ_cfg = {
        .occupancy          = 0,
        .sensor_type        = ESP_ZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_RESERVED,
        .sensor_type_bitmap = (1 << 2),
    };
    esp_zb_attribute_list_t *occ = esp_zb_occupancy_sensing_cluster_create(&occ_cfg);

    /* Zone config cluster: 4 attrs for this zone, pre-populated from NVS.
     * Static arrays persist for the lifetime of the stack (ZBoss holds pointers). */
    static uint8_t  s_zone_vc[ZB_EP_ZONE_COUNT];
    static char     s_zone_csv[ZB_EP_ZONE_COUNT][ZB_ZONE_COORDS_MAX_LEN];
    static uint16_t s_zone_cool[ZB_EP_ZONE_COUNT];
    static uint16_t s_zone_delay[ZB_EP_ZONE_COUNT];
    static bool     s_zone_inited = false;

    if (!s_zone_inited) {
        nvs_config_t cfg;
        nvs_config_get(&cfg);
        for (int n = 0; n < ZB_EP_ZONE_COUNT; n++) {
            s_zone_vc[n]   = cfg.zones[n].vertex_count;
            s_zone_cool[n] = cfg.occupancy_cooldown_sec[n + 1];
            s_zone_delay[n] = cfg.occupancy_delay_ms[n + 1];
            char tmp[ZB_ZONE_COORDS_MAX_LEN - 1];
            zone_to_csv(&cfg.zones[n], tmp, sizeof(tmp));
            size_t slen = strlen(tmp);
            /* Pre-fill to maximum length so ZBoss allocates the full internal buffer.
             * ZBoss allocates (length_byte + 1) bytes for its CHAR_STRING copy at
             * registration. Registering with only the current string length means a
             * longer update (more vertices) overflows into adjacent cooldown/delay
             * attribute storage. The padded initial value is corrected by
             * push_config_attrs() on the first poll after joining the network. */
            memset(s_zone_csv[n] + 1, ' ', ZB_ZONE_COORDS_MAX_LEN - 1);
            memcpy(s_zone_csv[n] + 1, tmp, slen);
            s_zone_csv[n][0] = (uint8_t)(ZB_ZONE_COORDS_MAX_LEN - 1);
        }
        s_zone_inited = true;
    }

    int n = zone_idx;
    esp_zb_attribute_list_t *zone_custom = esp_zb_zcl_attr_list_create(ZB_CLUSTER_LD2450_CONFIG);

    esp_zb_custom_cluster_add_custom_attr(zone_custom,
        ZB_ATTR_ZONE_VERTEX_COUNT(n),
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_zone_vc[n]);

    esp_zb_custom_cluster_add_custom_attr(zone_custom,
        ZB_ATTR_ZONE_COORDS(n),
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        s_zone_csv[n]);

    esp_zb_custom_cluster_add_custom_attr(zone_custom,
        ZB_ATTR_ZONE_COOLDOWN(n),
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_zone_cool[n]);

    esp_zb_custom_cluster_add_custom_attr(zone_custom,
        ZB_ATTR_ZONE_DELAY(n),
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_zone_delay[n]);

    /* Assemble */
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cl, identify, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_occupancy_sensing_cluster(cl, occ, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, zone_custom, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* On/Off CLIENT cluster on each zone EP — allows binding zone EP → light for fallback */
    esp_zb_attribute_list_t *on_off_client = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(cl, on_off_client, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));

    return cl;
}

/* ================================================================== */
/*  Endpoint registration (6 endpoints)                                */
/* ================================================================== */

static void zigbee_register_endpoints(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

    /* EP 1: Main device */
    esp_zb_endpoint_config_t main_ep_cfg = {
        .endpoint       = ZB_EP_MAIN,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id  = ZB_DEVICE_ID_OCCUPANCY_SENSOR,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, create_main_ep_clusters(), main_ep_cfg));

    /* EPs 2-11: Zone occupancy (occupancy sensing only — config lives on EP1) */
    for (int i = 0; i < ZB_EP_ZONE_COUNT; i++) {
        esp_zb_endpoint_config_t zone_ep_cfg = {
            .endpoint       = ZB_EP_ZONE(i),
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id  = ZB_DEVICE_ID_OCCUPANCY_SENSOR,
            .app_device_version = 0,
        };
        ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, create_zone_ep_clusters(i), zone_ep_cfg));
    }

    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));
    ESP_LOGI(TAG, "Registered %d endpoints (EP %d main + EP %d-%d zones)",
             1 + ZB_EP_ZONE_COUNT, ZB_EP_MAIN,
             ZB_EP_ZONE(0), ZB_EP_ZONE(ZB_EP_ZONE_COUNT - 1));
}

/* ================================================================== */
/*  Zigbee task                                                        */
/* ================================================================== */

static void zigbee_task(void *pv)
{
    (void)pv;

    esp_zb_platform_config_t platform_cfg = {0};
    platform_cfg.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE;
    platform_cfg.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE;

    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    /* Increase binding table: EP1 (2) + 10 zone EPs (10) = 12 minimum; 24 gives headroom */
    esp_zb_aps_src_binding_table_size_set(24);
    esp_zb_aps_dst_binding_table_size_set(24);

    esp_zb_cfg_t zb_cfg = {0};
    zb_cfg.esp_zb_role = (
    #if CONFIG_LD2450_ZB_ROUTER
        ESP_ZB_DEVICE_TYPE_ROUTER
    #else
        ESP_ZB_DEVICE_TYPE_ED
    #endif
    );
    #if CONFIG_LD2450_ZB_ROUTER
    zb_cfg.nwk_cfg.zczr_cfg.max_children = 10;
    #endif

    esp_zb_init(&zb_cfg);

    /* Register action handler before endpoint registration */
    esp_zb_core_action_handler_register(zigbee_action_handler);

    zigbee_register_endpoints();

    /* Init fallback module after endpoints are registered (needs ZCL attrs to be present) */
    coordinator_fallback_init();

    /* Start Zigbee stack with graceful error handling - avoid reboot on network failure */
    esp_err_t err = esp_zb_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee start failed: %s, continuing in pairing mode...", esp_err_to_name(err));
        board_led_set_state_not_joined();  /* Show pairing state */
        /* Don't abort - let signal handler retry steering */
    }
    /* Continue to main loop regardless - steering retry will handle network connection */
    esp_zb_stack_main_loop();
}

/* ================================================================== */
/*  Sync zone attrs from NVS after stack starts                        */
/* ================================================================== */

void zigbee_sync_zone_attrs_from_nvs(void)
{
    nvs_config_t cfg;
    nvs_config_get(&cfg);

    for (int n = 0; n < ZB_EP_ZONE_COUNT; n++) {
        const ld2450_zone_t *z = &cfg.zones[n];
        uint8_t ep = ZB_EP_ZONE(n);

        /* vertex_count */
        uint8_t vc = z->vertex_count;
        esp_zb_zcl_set_attribute_val(ep, ZB_CLUSTER_LD2450_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_ZONE_VERTEX_COUNT(n), &vc, false);

        /* coords CSV — ZCL CHAR_STRING: first byte = length */
        char csv[ZB_ZONE_COORDS_MAX_LEN - 1];
        zone_to_csv(z, csv, sizeof(csv));
        uint8_t zcl_str[ZB_ZONE_COORDS_MAX_LEN];
        zcl_str[0] = (uint8_t)strlen(csv);
        memcpy(zcl_str + 1, csv, zcl_str[0]);
        esp_zb_zcl_set_attribute_val(ep, ZB_CLUSTER_LD2450_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_ZONE_COORDS(n), zcl_str, false);

        /* cooldown and delay (index n+1: 0=main EP, 1-10=zones) */
        uint16_t cool  = cfg.occupancy_cooldown_sec[n + 1];
        uint16_t delay = cfg.occupancy_delay_ms[n + 1];
        esp_zb_zcl_set_attribute_val(ep, ZB_CLUSTER_LD2450_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_ZONE_COOLDOWN(n), &cool, false);
        esp_zb_zcl_set_attribute_val(ep, ZB_CLUSTER_LD2450_CONFIG,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_ZONE_DELAY(n), &delay, false);
    }

    ESP_LOGI(TAG, "Zone attrs synced from NVS");
}

/* ================================================================== */
/*  Entry point                                                        */
/* ================================================================== */

void zigbee_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "LD2450 Firmware Version: v" ZB_FW_VERSION_STR);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Starting Zigbee task...");
    xTaskCreate(zigbee_task, "zb_task", 8192, NULL, 5, NULL);
}
