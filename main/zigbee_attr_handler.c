// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_zigbee_core.h"

/* Project */
#include "ld2450.h"
#include "ld2450_cmd.h"
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
        case ZB_ATTR_MAX_DISTANCE: {
            uint16_t dist = *(uint16_t *)val;
            esp_err_t err = nvs_config_save_max_distance(dist);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save max_distance to NVS: %s", esp_err_to_name(err));
            }
            nvs_config_t cfg;
            nvs_config_get(&cfg);
            ld2450_cmd_apply_distance_angle(cfg.max_distance_mm, cfg.angle_left_deg, cfg.angle_right_deg);
            ESP_LOGI(TAG, "Max distance -> %u mm%s", dist, (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");
            return ESP_OK;
        }
        case ZB_ATTR_ANGLE_LEFT: {
            uint8_t deg = *(uint8_t *)val;
            esp_err_t err = nvs_config_save_angle_left(deg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save angle_left to NVS: %s", esp_err_to_name(err));
            }
            nvs_config_t cfg;
            nvs_config_get(&cfg);
            ld2450_cmd_apply_distance_angle(cfg.max_distance_mm, cfg.angle_left_deg, cfg.angle_right_deg);
            ESP_LOGI(TAG, "Angle left -> %u%s", deg, (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");
            return ESP_OK;
        }
        case ZB_ATTR_ANGLE_RIGHT: {
            uint8_t deg = *(uint8_t *)val;
            esp_err_t err = nvs_config_save_angle_right(deg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save angle_right to NVS: %s", esp_err_to_name(err));
            }
            nvs_config_t cfg;
            nvs_config_get(&cfg);
            ld2450_cmd_apply_distance_angle(cfg.max_distance_mm, cfg.angle_left_deg, cfg.angle_right_deg);
            ESP_LOGI(TAG, "Angle right -> %u%s", deg, (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");
            return ESP_OK;
        }
        case ZB_ATTR_TRACKING_MODE: {
            uint8_t mode = *(uint8_t *)val;
            ld2450_set_tracking_mode(mode ? LD2450_TRACK_SINGLE : LD2450_TRACK_MULTI);
            esp_err_t err = nvs_config_save_tracking_mode(mode);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save tracking_mode to NVS: %s", esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "Tracking mode -> %s%s", mode ? "single" : "multi", (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");
            return ESP_OK;
        }
        case ZB_ATTR_COORD_PUBLISHING: {
            uint8_t en = *(uint8_t *)val;
            ld2450_set_publish_coords(en != 0);
            esp_err_t err = nvs_config_save_publish_coords(en);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save publish_coords to NVS: %s", esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "Coord publishing -> %s%s", en ? "on" : "off", (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");
            return ESP_OK;
        }
        case ZB_ATTR_OCCUPANCY_COOLDOWN: {
            uint16_t sec = *(uint16_t *)val;
            esp_err_t err = nvs_config_save_occupancy_cooldown(0, sec);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save main occupancy_cooldown to NVS: %s", esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "Main occupancy cooldown -> %u sec%s", sec, (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");
            return ESP_OK;
        }
        case ZB_ATTR_OCCUPANCY_DELAY: {
            uint16_t ms = *(uint16_t *)val;
            esp_err_t err = nvs_config_save_occupancy_delay(0, ms);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save main occupancy_delay to NVS: %s", esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "Main occupancy delay -> %u ms%s", ms, (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");
            return ESP_OK;
        }
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

    /* EP1 zone config attributes (0x0040-0x006B) on cluster 0xFC00 */
    if (ep == ZB_EP_MAIN && cluster == ZB_CLUSTER_LD2450_CONFIG
            && attr_id >= 0x0040 && attr_id <= 0x006B) {

        int n   = (attr_id - 0x0040) / 4;   /* zone index 0..9 */
        int sub = (attr_id - 0x0040) % 4;   /* 0=vertex_count, 1=coords, 2=cooldown, 3=delay */

        nvs_config_t cfg;
        nvs_config_get(&cfg);

        if (sub == 0) {
            /* vertex_count write */
            uint8_t vc = *(uint8_t *)val;
            if (vc > MAX_ZONE_VERTICES) vc = 0;  /* clamp invalid to disabled */
            cfg.zones[n].vertex_count = vc;
            ld2450_set_zone((size_t)n, &cfg.zones[n]);
            esp_err_t err = nvs_config_save_zone((uint8_t)n, &cfg.zones[n]);
            ESP_LOGI(TAG, "zone_%d vertex_count -> %u%s", n + 1, vc,
                     (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");

        } else if (sub == 1) {
            /* coords CSV write — validate pair count before applying */
            uint8_t *zcl_str = (uint8_t *)val;
            uint8_t len = zcl_str[0];
            char csv[ZB_ZONE_COORDS_MAX_LEN];
            if (len >= ZB_ZONE_COORDS_MAX_LEN) len = ZB_ZONE_COORDS_MAX_LEN - 1;
            memcpy(csv, zcl_str + 1, len);
            csv[len] = '\0';

            int pairs = csv_count_pairs(csv);
            if (pairs != cfg.zones[n].vertex_count) {
                ESP_LOGW(TAG, "zone_%d coords rejected: expected %d pairs, got %d — reverting",
                         n + 1, cfg.zones[n].vertex_count, pairs);
                /* Write stored value back so Z2M sees the revert */
                char revert_csv[ZB_ZONE_COORDS_MAX_LEN - 1];
                zone_to_csv(&cfg.zones[n], revert_csv, sizeof(revert_csv));
                uint8_t revert_zcl[ZB_ZONE_COORDS_MAX_LEN];
                revert_zcl[0] = (uint8_t)strlen(revert_csv);
                memcpy(revert_zcl + 1, revert_csv, revert_zcl[0]);
                esp_zb_zcl_set_attribute_val(ZB_EP_MAIN, ZB_CLUSTER_LD2450_CONFIG,
                    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ATTR_ZONE_COORDS(n), revert_zcl, false);
                return ESP_OK;
            }
            csv_to_zone(csv, &cfg.zones[n]);
            ld2450_set_zone((size_t)n, &cfg.zones[n]);
            esp_err_t err = nvs_config_save_zone((uint8_t)n, &cfg.zones[n]);
            ESP_LOGI(TAG, "zone_%d coords -> \"%s\"%s", n + 1, csv,
                     (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");

        } else if (sub == 2) {
            /* cooldown write (index n+1 in array: 0=main EP, 1-10=zones) */
            uint16_t sec = *(uint16_t *)val;
            esp_err_t err = nvs_config_save_occupancy_cooldown((uint8_t)(n + 1), sec);
            ESP_LOGI(TAG, "zone_%d cooldown -> %u sec%s", n + 1, sec,
                     (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");

        } else {
            /* delay write */
            uint16_t ms = *(uint16_t *)val;
            esp_err_t err = nvs_config_save_occupancy_delay((uint8_t)(n + 1), ms);
            ESP_LOGI(TAG, "zone_%d delay -> %u ms%s", n + 1, ms,
                     (err == ESP_OK) ? " (saved)" : " (NVS FAILED)");
        }
        return ESP_OK;
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
