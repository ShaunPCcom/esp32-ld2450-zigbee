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
#include "crash_diag.h"
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
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &init_dist);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_ANGLE_LEFT,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &init_al);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_ANGLE_RIGHT,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &init_ar);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_TRACKING_MODE,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &init_mode);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_COORD_PUBLISHING,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &init_coords);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_OCCUPANCY_COOLDOWN,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &init_cooldown);

    esp_zb_custom_cluster_add_custom_attr(custom, ZB_ATTR_OCCUPANCY_DELAY,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
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

    /* Assemble cluster list */
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cl, identify, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_occupancy_sensing_cluster(cl, occ, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, custom, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* Add OTA cluster */
    zigbee_ota_config_t ota_cfg = ZIGBEE_OTA_CONFIG_DEFAULT();
    ota_cfg.manufacturer_code = 0x131B;  /* Espressif */
    ota_cfg.image_type = 0x0001;         /* LD2450 application */
    ota_cfg.current_file_version = FIRMWARE_VERSION;  /* Derived from version.h */
    ota_cfg.hw_version = 1;
    ota_cfg.query_interval_minutes = 1440;  /* Check every 24 hours */
    ESP_ERROR_CHECK(zigbee_ota_init(cl, ZB_EP_MAIN, &ota_cfg));

    return cl;
}

static esp_zb_cluster_list_t *create_zone_ep_clusters(uint8_t zone_idx)
{
    /* Basic cluster (minimal for zone endpoints) */
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

    /* Custom cluster 0xFC01 - Zone vertex config */
    esp_zb_attribute_list_t *zone_custom = esp_zb_zcl_attr_list_create(ZB_CLUSTER_LD2450_ZONE);

    /* Load zone vertices from NVS */
    nvs_config_t cfg;
    nvs_config_get(&cfg);
    const ld2450_zone_t *z = &cfg.zones[zone_idx];

    for (int v = 0; v < ZB_ATTR_ZONE_VERTEX_COUNT; v++) {
        int16_t val;
        int vi = v / 2;  /* vertex index 0-3 */
        if (v % 2 == 0) {
            val = z->v[vi].x_mm;
        } else {
            val = z->v[vi].y_mm;
        }
        esp_zb_custom_cluster_add_custom_attr(zone_custom, (uint16_t)v,
            ESP_ZB_ZCL_ATTR_TYPE_S16,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &val);
    }

    /* Add occupancy cooldown attribute for this zone */
    uint16_t zone_cooldown = cfg.occupancy_cooldown_sec[zone_idx + 1];
    esp_zb_custom_cluster_add_custom_attr(zone_custom, ZB_ATTR_OCCUPANCY_COOLDOWN,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &zone_cooldown);

    /* Add occupancy delay attribute for this zone */
    uint16_t zone_delay = cfg.occupancy_delay_ms[zone_idx + 1];
    esp_zb_custom_cluster_add_custom_attr(zone_custom, ZB_ATTR_OCCUPANCY_DELAY,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &zone_delay);

    /* Assemble cluster list */
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cl, identify, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_occupancy_sensing_cluster(cl, occ, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cl, zone_custom, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

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

    /* EPs 2-6: Zone occupancy */
    for (int i = 0; i < ZB_EP_ZONE_COUNT; i++) {
        esp_zb_endpoint_config_t zone_ep_cfg = {
            .endpoint       = ZB_EP_ZONE(i),
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id  = ZB_DEVICE_ID_OCCUPANCY_SENSOR,
            .app_device_version = 0,
        };
        ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, create_zone_ep_clusters((uint8_t)i), zone_ep_cfg));
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

    esp_zb_cfg_t zb_cfg = {0};
    zb_cfg.esp_zb_role = (
    #if CONFIG_LD2450_ZB_ROUTER
        ESP_ZB_DEVICE_TYPE_ROUTER
    #else
        ESP_ZB_DEVICE_TYPE_ED
    #endif
    );

    esp_zb_init(&zb_cfg);

    /* Register action handler before endpoint registration */
    esp_zb_core_action_handler_register(zigbee_action_handler);

    zigbee_register_endpoints();

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
