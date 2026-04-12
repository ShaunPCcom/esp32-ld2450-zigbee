// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_zigbee_core.h"

/* Project */
#include "config_api.h"
#include "crash_diag.h"
#include "ld2450_zone_csv.h"
#include "nvs_config.h"
#include "zigbee_attr_handler.h"
#include "zigbee_ctrl.h"
#include "zigbee_defs.h"
#include "zigbee_ota.h"

static const char *TAG = "zigbee_attr";

/* ================================================================== */
/*  Action handler (writable attribute callbacks)                      */
/* ================================================================== */

static esp_err_t handle_set_attr_value(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    uint8_t ep = msg->info.dst_endpoint;
    uint16_t cluster = msg->info.cluster;
    uint16_t attr_id = msg->attribute.id;
    void *val = msg->attribute.data.value;

    ESP_LOGI(TAG, "Write: ep=%u cluster=0x%04X attr=0x%04X", ep, cluster, attr_id);

    /* EP 1 custom cluster */
    if (ep == ZB_EP_MAIN && cluster == ZB_CLUSTER_LD2450_CONFIG) {
        switch (attr_id) {
        case ZB_ATTR_MAX_DISTANCE:
            return config_api_set_max_distance(*(uint16_t *)val);
        case ZB_ATTR_ANGLE_LEFT:
            return config_api_set_angle_left(*(uint8_t *)val);
        case ZB_ATTR_ANGLE_RIGHT:
            return config_api_set_angle_right(*(uint8_t *)val);
        case ZB_ATTR_TRACKING_MODE:
            return config_api_set_tracking_mode(*(uint8_t *)val);
        case ZB_ATTR_COORD_PUBLISHING:
            return config_api_set_publish_coords(*(uint8_t *)val);
        case ZB_ATTR_OCCUPANCY_COOLDOWN:
            return config_api_set_occupancy_cooldown(0, *(uint16_t *)val);
        case ZB_ATTR_OCCUPANCY_DELAY:
            return config_api_set_occupancy_delay(0, *(uint16_t *)val);
        case ZB_ATTR_FALLBACK_MODE:
            return config_api_set_fallback_mode(*(uint8_t *)val);
        case ZB_ATTR_FALLBACK_COOLDOWN:
            return config_api_set_fallback_cooldown(0, *(uint16_t *)val);
        case ZB_ATTR_HEARTBEAT_ENABLE:
            return config_api_set_heartbeat_enable(*(uint8_t *)val);
        case ZB_ATTR_HEARTBEAT_INTERVAL:
            return config_api_set_heartbeat_interval(*(uint16_t *)val);
        case ZB_ATTR_HEARTBEAT:
            return config_api_heartbeat();
        case ZB_ATTR_FALLBACK_ENABLE:
            return config_api_set_fallback_enable(*(uint8_t *)val);
        case ZB_ATTR_HARD_TIMEOUT_SEC:
            return config_api_set_hard_timeout(*(uint8_t *)val);
        case ZB_ATTR_ACK_TIMEOUT_MS:
            return config_api_set_ack_timeout(*(uint16_t *)val);
        case ZB_ATTR_DIAG_RESET:
            if (*(uint8_t *)val) crash_diag_reset_boot_count();
            return ESP_OK;
        case ZB_ATTR_RESTART:
            zgb_ctrl_handle_restart();
            return ESP_OK;
        case ZB_ATTR_FACTORY_RESET: {
            extern void zigbee_full_factory_reset(void);
            zgb_ctrl_handle_factory_reset(*(uint8_t *)val, zigbee_full_factory_reset);
            return ESP_OK;
        }
        default:
            break;
        }
    }

    /* EP1 fallback zone cooldown attributes (0x0070-0x0079) on cluster 0xFC00 */
    if (ep == ZB_EP_MAIN && cluster == ZB_CLUSTER_LD2450_CONFIG
            && attr_id >= ZB_ATTR_FALLBACK_ZONE_COOL_BASE
            && attr_id <= ZB_ATTR_FALLBACK_ZONE_COOL_BASE + 9) {
        uint8_t zone_idx = (uint8_t)(attr_id - ZB_ATTR_FALLBACK_ZONE_COOL_BASE);
        return config_api_set_fallback_cooldown((uint8_t)(zone_idx + 1), *(uint16_t *)val);
    }

    /* Zone EP config attributes on cluster 0xFC00 (EP2-EP11, one zone per EP) */
    if (ep >= ZB_EP_ZONE_BASE && ep < ZB_EP_ZONE_BASE + ZB_EP_ZONE_COUNT
            && cluster == ZB_CLUSTER_LD2450_CONFIG) {

        int n        = ep - ZB_EP_ZONE_BASE;               /* zone index 0..9 */
        uint16_t base = ZB_ZONE_ATTR_BASE(n);
        if (attr_id < base || attr_id > base + 3) return ESP_OK;
        int sub = attr_id - base;  /* 0=vertex_count, 1=coords, 2=cooldown, 3=delay */

        if (sub == 0) {
            return config_api_set_zone_vertex_count((uint8_t)n, *(uint8_t *)val);

        } else if (sub == 1) {
            /* Extract CSV from ZCL CHAR_STRING (length-prefixed) */
            uint8_t *zcl_str = (uint8_t *)val;
            uint8_t len = zcl_str[0];
            char csv[ZB_ZONE_COORDS_MAX_LEN];
            if (len >= ZB_ZONE_COORDS_MAX_LEN) {
                len = ZB_ZONE_COORDS_MAX_LEN - 1;
            }
            memcpy(csv, zcl_str + 1, len);
            csv[len] = '\0';

            esp_err_t err = config_api_set_zone_coords((uint8_t)n, csv);
            if (err == ESP_ERR_INVALID_ARG) {
                /* Pair count mismatch — revert ZCL attribute to stored value */
                nvs_config_t cfg;
                nvs_config_get(&cfg);
                char revert_csv[ZB_ZONE_COORDS_MAX_LEN - 1];
                zone_to_csv(&cfg.zones[n], revert_csv, sizeof(revert_csv));
                uint8_t revert_zcl[ZB_ZONE_COORDS_MAX_LEN];
                revert_zcl[0] = (uint8_t)strlen(revert_csv);
                memcpy(revert_zcl + 1, revert_csv, revert_zcl[0]);
                esp_zb_zcl_set_attribute_val(ZB_EP_ZONE(n), ZB_CLUSTER_LD2450_CONFIG,
                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_ZONE_COORDS(n), revert_zcl, false);
            }
            return ESP_OK;

        } else if (sub == 2) {
            return config_api_set_occupancy_cooldown((uint8_t)(n + 1), *(uint16_t *)val);

        } else {
            return config_api_set_occupancy_delay((uint8_t)(n + 1), *(uint16_t *)val);
        }
    }

    return ESP_OK;
}

esp_err_t zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    /* Route OTA callbacks to OTA component */
    esp_err_t ret = zigbee_ota_action_handler(callback_id, message);
    if (ret != ESP_ERR_NOT_SUPPORTED) {
        return ret;  /* OTA component handled it */
    }

    /* Handle application callbacks */
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return handle_set_attr_value((const esp_zb_zcl_set_attr_value_message_t *)message);
    }
    return ESP_OK;
}
