// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_zigbee_core.h"

/* Project */
#include "board_led.h"
#include "nvs_config.h"
#include "sensor_bridge.h"
#include "zigbee_defs.h"
#include "zigbee_init.h"
#include "zigbee_signal_handlers.h"

static const char *TAG = "zigbee_signal";

/* Network join state */
static bool s_network_joined = false;

/* ================================================================== */
/*  Signal handler                                                     */
/* ================================================================== */

static void steering_retry_cb(uint8_t param)
{
    board_led_set_state_pairing();
    esp_zb_bdb_start_top_level_commissioning(param);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct ? signal_struct->p_app_signal : NULL;
    esp_zb_app_signal_type_t sig = p_sg_p ? *p_sg_p : 0;
    esp_err_t status = signal_struct ? signal_struct->esp_err_status : ESP_OK;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Stack initialized, starting steering");
        board_led_set_state_pairing();
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory new device, starting steering");
                board_led_set_state_pairing();
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted, already commissioned");
                board_led_set_state_joined();
                s_network_joined = true;
                zigbee_sync_zone_attrs_from_nvs();
                sensor_bridge_start();
            }
        } else {
            ESP_LOGW(TAG, "Device start/reboot failed: %s", esp_err_to_name(status));
            board_led_set_state_error();
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Joined network successfully");
            board_led_set_state_joined();
            s_network_joined = true;
            zigbee_sync_zone_attrs_from_nvs();
            sensor_bridge_start();
        } else {
            ESP_LOGW(TAG, "Steering failed (%s), retrying...", esp_err_to_name(status));
            board_led_set_state_not_joined();
            esp_zb_scheduler_alarm(steering_retry_cb,
                                   ESP_ZB_BDB_NETWORK_STEERING, 1000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "Left network");
        s_network_joined = false;
        board_led_set_state_not_joined();
        esp_zb_scheduler_alarm(steering_retry_cb,
                               ESP_ZB_BDB_NETWORK_STEERING, 1000);
        break;

    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        break;

    default:
        ESP_LOGI(TAG, "ZB signal=0x%08" PRIx32 " status=%s",
                 (uint32_t)sig, esp_err_to_name(status));
        break;
    }
}

/* ================================================================== */
/*  Factory reset (callable from any context)                          */
/* ================================================================== */

bool zigbee_is_network_joined(void)
{
    return s_network_joined;
}

void zigbee_factory_reset(void)
{
    ESP_LOGW(TAG, "Zigbee network reset - leaving network, keeping config");
    board_led_set_state_error();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_zb_factory_reset();
    /* esp_zb_factory_reset() restarts, but just in case: */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void zigbee_full_factory_reset(void)
{
    ESP_LOGW(TAG, "FULL factory reset - erasing Zigbee network + NVS config");
    board_led_set_state_error();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Erase application NVS namespace */
    nvs_handle_t h;
    if (nvs_open("ld2450_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS config erased");
    }

    /* Then erase Zigbee network data and restart */
    esp_zb_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
