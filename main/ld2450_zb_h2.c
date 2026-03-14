// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "board_config.h"
#include "ld2450.h"
#include "ld2450_cmd.h"
#include "ld2450_cli.h"
#include "nvs_config.h"
#include "sdkconfig.h"
#include "zigbee_app.h"
#include "board_led.h"

static const char *TAG = "ld2450_main";

static void apply_saved_config(const nvs_config_t *cfg)
{
    /* Apply software config to driver */
    ld2450_set_tracking_mode(cfg->tracking_mode == 1 ? LD2450_TRACK_SINGLE : LD2450_TRACK_MULTI);
    ld2450_set_publish_coords(cfg->publish_coords != 0);

    /* Load saved zones individually — batch set_zones rejects all if any zone
     * has vertex_count>=3 with all-zero coords (e.g. Z2M auto-populated placeholder).
     * Per-zone calls let valid zones load while placeholders stay disabled. */
    for (int i = 0; i < 10; i++) {
        ld2450_set_zone((size_t)i, &cfg->zones[i]);
    }

    /* Allow sensor time to boot before sending commands */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Apply hardware config via sensor commands */
    if (cfg->bt_disabled) {
        ld2450_cmd_set_bluetooth(false);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ld2450_cmd_apply_distance_angle(cfg->max_distance_mm,
                                     cfg->angle_left_deg,
                                     cfg->angle_right_deg);

    ESP_LOGI(TAG, "Saved config applied");
}

void app_main(void)
{
    board_led_init();
    board_led_set_state(BOARD_LED_NOT_JOINED);
    ESP_LOGI(TAG, "Zigbee role: %s", CONFIG_LD2450_ZB_ROUTER ? "router" : "end device");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    /* Load persistent config (or defaults) */
    ESP_ERROR_CHECK(nvs_config_init());

    nvs_config_t saved_cfg;
    ESP_ERROR_CHECK(nvs_config_get(&saved_cfg));

    ld2450_config_t cfg = {
        .uart_num     = LD2450_UART_NUM,
        .tx_gpio      = LD2450_UART_TX_GPIO,
        .rx_gpio      = LD2450_UART_RX_GPIO,
        .baud_rate    = LD2450_UART_BAUD,
        .rx_buf_size  = 2048,
    };

    ESP_ERROR_CHECK(ld2450_init(&cfg));
    ESP_ERROR_CHECK(ld2450_cmd_init());

    /* Apply saved config (zones, hardware params) */
    apply_saved_config(&saved_cfg);

    /* Bring up CLI early so we can debug even if Zigbee gets noisy */
    ld2450_cli_start();

    /* Zigbee bring-up */
    zigbee_app_start();

    ESP_LOGI(TAG, "LD2450 initialized.");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
