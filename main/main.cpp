// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

extern "C" {
#include "crash_diag.h"
#include "ld2450.h"
#include "ld2450_cmd.h"
#include "ld2450_cli.h"
#include "nvs_config.h"
#include "zigbee_init.h"
#include "zigbee_signal_handlers.h"
}

#include "sdkconfig.h"

/* C++ shared components */
#include "project_defaults.hpp"
#include "board_led.hpp"
#include "zigbee_button.hpp"

using namespace defaults;

static const char *TAG = "ld2450_main";

/* Global instances of C++ shared components */
static BoardLed *g_board_led = nullptr;
static ButtonHandler *g_button = nullptr;

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

extern "C" void app_main(void)
{
    /* Initialize board LED (C++ BoardLed class) */
    g_board_led = new BoardLed(BOARD_LED_GPIO);
    g_board_led->set_state(BoardLed::State::NOT_JOINED);
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

    /* Initialize crash diagnostics (must be early for accurate reset reason) */
    ESP_ERROR_CHECK(crash_diag_init());

    nvs_config_t saved_cfg;
    ESP_ERROR_CHECK(nvs_config_get(&saved_cfg));

    ld2450_config_t cfg = {
        .uart_num     = (uart_port_t)LD2450_UART_NUM,
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
    zigbee_init();

    /* Initialize button handler (C++ ButtonHandler class) */
    g_button = new ButtonHandler(BOARD_BUTTON_GPIO,
                                  BOARD_BUTTON_HOLD_ZIGBEE_MS,
                                  BOARD_BUTTON_HOLD_FULL_MS);
    g_button->set_network_reset_callback(zigbee_factory_reset);
    g_button->set_full_reset_callback(zigbee_full_factory_reset);
    g_button->set_led_callback([](int state) {
        switch (state) {
            case 0: /* Restore previous state */
                g_board_led->set_state(zigbee_is_network_joined() ? BoardLed::State::JOINED : BoardLed::State::NOT_JOINED);
                break;
            case 1: /* Amber (NOT_JOINED) */
                g_board_led->set_state(BoardLed::State::NOT_JOINED);
                break;
            case 2: /* Red (ERROR) */
                g_board_led->set_state(BoardLed::State::ERROR);
                break;
        }
    });
    g_button->start();
    ESP_LOGI(TAG, "Button handler started (GPIO %d)", BOARD_BUTTON_GPIO);

    ESP_LOGI(TAG, "LD2450 initialized.");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

/* C wrappers for C++ BoardLed API (called from C modules) */
extern "C" void board_led_set_state_not_joined(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::NOT_JOINED);
}

extern "C" void board_led_set_state_pairing(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::PAIRING);
}

extern "C" void board_led_set_state_joined(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::JOINED);
}

extern "C" void board_led_set_state_error(void) {
    if (g_board_led) g_board_led->set_state(BoardLed::State::ERROR);
}
