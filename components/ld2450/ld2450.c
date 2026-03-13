// SPDX-License-Identifier: MIT
#include "ld2450.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include <inttypes.h>

#include "ld2450_parser.h"
#include "ld2450_zone.h"

#define LD2450_ZONE_COUNT 10
#define ZONE_ID_USER(z) ((z) + 1)

static ld2450_zone_t s_zones[LD2450_ZONE_COUNT] = {
    /* vertex_count < 3 = disabled (all 10 zones start disabled) */
    { .vertex_count = 0 }, { .vertex_count = 0 }, { .vertex_count = 0 },
    { .vertex_count = 0 }, { .vertex_count = 0 }, { .vertex_count = 0 },
    { .vertex_count = 0 }, { .vertex_count = 0 }, { .vertex_count = 0 },
    { .vertex_count = 0 },
};

static const char *TAG = "ld2450";

static TaskHandle_t s_uart_task = NULL;
static uart_port_t s_uart_num = UART_NUM_MAX;
static volatile bool s_rx_pause_requested = false;
static SemaphoreHandle_t s_rx_paused_sem = NULL;  // signaled when RX task has paused

// Protects s_zones, runtime cfg, and state snapshots
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static ld2450_runtime_cfg_t s_cfg = {
    .enabled = true,
    .mode = LD2450_TRACK_MULTI,
    .publish_coords = false,
};

static ld2450_state_t s_state = {0};

static bool zone_vertices_sane(const ld2450_zone_t *z)
{
    // Disabled zones are always sane.
    if (z->vertex_count < 3) return true;
    for (int i = 0; i < z->vertex_count; i++) {
        if (z->v[i].x_mm != 0 || z->v[i].y_mm != 0) return true;
    }
    return false;
}

static ld2450_target_t select_single_target(const ld2450_report_t *r)
{
    // Policy: pick closest (smallest positive y_mm). If no positive y, pick smallest |y|.
    bool have = false;
    ld2450_target_t best = {0};
    int best_y = 0;

    for (unsigned i = 0; i < r->target_count && i < 3; i++) {
        const ld2450_target_t *t = &r->targets[i];
        if (!t->present) continue;

        if (!have) {
            best = *t;
            best_y = t->y_mm;
            have = true;
            continue;
        }

        bool best_pos = best_y > 0;
        bool cur_pos  = t->y_mm > 0;

        if (cur_pos && !best_pos) {
            best = *t;
            best_y = t->y_mm;
            continue;
        }

        if (cur_pos && best_pos) {
            if (t->y_mm < best_y) {
                best = *t;
                best_y = t->y_mm;
            }
            continue;
        }

        // both non-positive: choose smallest absolute y
        if (abs(t->y_mm) < abs(best_y)) {
            best = *t;
            best_y = t->y_mm;
        }
    }

    return best;
}

static void ld2450_uart_task(void *arg)
{
    const int buf_len = 256;
    uint8_t buf[buf_len];

    ESP_LOGI(TAG, "UART task started on uart=%d", (int)s_uart_num);

    ld2450_parser_t *parser = ld2450_parser_create();
    if (!parser) {
        ESP_LOGE(TAG, "ld2450_parser_create failed");
        vTaskDelete(NULL);
        return;
    }

    ld2450_report_t last = {0};
    bool have_last = false;

    while (1) {
        // If command module requested pause, yield until resumed
        if (s_rx_pause_requested) {
            xSemaphoreGive(s_rx_paused_sem);  // signal "I'm paused"
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // block until resumed
            continue;
        }

        // Block up to 100ms waiting for data (short so pause requests aren't delayed)
        int n = uart_read_bytes(s_uart_num, buf, buf_len, pdMS_TO_TICKS(100));
        if (n > 0) {
            if (ld2450_parser_feed(parser, buf, (size_t)n)) {
                const ld2450_report_t *r = ld2450_parser_get_report(parser);

                // Snapshot runtime cfg
                ld2450_runtime_cfg_t cfg;
                portENTER_CRITICAL(&s_lock);
                cfg = s_cfg;
                portEXIT_CRITICAL(&s_lock);

                bool changed = !have_last || memcmp(&last, r, sizeof(*r)) != 0;
                if (changed && cfg.enabled) {
                    ESP_LOGI(TAG, "report: occupied=%d target_count=%u",
                             (int)r->occupied, (unsigned)r->target_count);

                    for (unsigned i = 0; i < r->target_count && i < 3; i++) {
                        const ld2450_target_t *t = &r->targets[i];
                        ESP_LOGI(TAG,
                                 "  T%u: present=%d x_mm=%d y_mm=%d speed=%d",
                                 i, (int)t->present, (int)t->x_mm, (int)t->y_mm, (int)t->speed);
                    }
                }

                // Determine effective targets for single-target mode
                ld2450_target_t selected = (ld2450_target_t){0};
                uint8_t eff_count = 0;
                if (r->occupied) {
                    if (cfg.mode == LD2450_TRACK_SINGLE) {
                        selected = select_single_target(r);
                        eff_count = 1;
                    } else {
                        // Multi: pick first present as "selected" (for debug UI later)
                        for (unsigned i = 0; i < r->target_count && i < 3; i++) {
                            if (r->targets[i].present) { selected = r->targets[i]; break; }
                        }
                        eff_count = r->target_count;
                    }
                }

                // ---- Zone evaluation ----
                bool zone_occ[LD2450_ZONE_COUNT] = {0};

                if (cfg.enabled && r->occupied) {
                    for (unsigned zi = 0; zi < LD2450_ZONE_COUNT; zi++) {
                        /* ld2450_zone_contains_point returns false when vertex_count < 3 */

                        if (cfg.mode == LD2450_TRACK_SINGLE) {
                            ld2450_point_t p = { .x_mm = selected.x_mm, .y_mm = selected.y_mm };
                            if (ld2450_zone_contains_point(&s_zones[zi], p)) {
                                zone_occ[zi] = true;
                            }
                            continue;
                        }

                        for (unsigned ti = 0; ti < r->target_count && ti < 3; ti++) {
                            const ld2450_target_t *t = &r->targets[ti];
                            if (!t->present)
                                continue;

                            ld2450_point_t p = { .x_mm = t->x_mm, .y_mm = t->y_mm };
                            if (ld2450_zone_contains_point(&s_zones[zi], p)) {
                                zone_occ[zi] = true;
                                break;
                            }
                        }
                    }
                }

                // ---- Zone change logging + bitmap ----
                static bool last_zone_occ[LD2450_ZONE_COUNT] = {0};
                uint16_t zone_bitmap = 0;

                for (unsigned zi = 0; zi < LD2450_ZONE_COUNT; zi++) {
                    if (zone_occ[zi]) zone_bitmap |= (1u << zi);
                }

                if (cfg.enabled) {
                    for (unsigned zi = 0; zi < LD2450_ZONE_COUNT; zi++) {
                        if (zone_occ[zi] != last_zone_occ[zi]) {
                            ESP_LOGI(TAG, "zone%u: %s", ZONE_ID_USER(zi), zone_occ[zi] ? "occupied" : "clear");
                            last_zone_occ[zi] = zone_occ[zi];
                        }
                    }
                }

                // Export state snapshot (even if logging disabled)
                portENTER_CRITICAL(&s_lock);
                s_state.occupied_global = r->occupied;
                s_state.target_count_raw = r->target_count;
                s_state.target_count_effective = eff_count;
                s_state.selected = selected;
                memcpy(s_state.targets, r->targets, sizeof(s_state.targets));
                memcpy(s_state.zone_occupied, zone_occ, sizeof(s_state.zone_occupied));
                s_state.zone_bitmap = zone_bitmap;
                portEXIT_CRITICAL(&s_lock);

                last = *r;        // struct copy
                have_last = true;
            }
        }
    }
}

esp_err_t ld2450_init(const ld2450_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->uart_num >= UART_NUM_MAX) return ESP_ERR_INVALID_ARG;
    if (cfg->rx_gpio < 0 || cfg->tx_gpio < 0) return ESP_ERR_INVALID_ARG;

    if (s_uart_task) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_uart_num = cfg->uart_num;

    uart_config_t uart_cfg = {
        .baud_rate = cfg->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(
        s_uart_num,
        cfg->rx_buf_size > 0 ? cfg->rx_buf_size : 2048,
        256,    // TX buffer for sending commands to sensor
        0, NULL,
        0
    ));
    ESP_ERROR_CHECK(uart_param_config(s_uart_num, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(s_uart_num, cfg->tx_gpio, cfg->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "Configured UART%d: baud=%d tx=%d rx=%d",
             (int)s_uart_num, cfg->baud_rate, cfg->tx_gpio, cfg->rx_gpio);

    s_rx_paused_sem = xSemaphoreCreateBinary();
    if (!s_rx_paused_sem) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(ld2450_uart_task, "ld2450_uart", 4096, NULL, 10, &s_uart_task);
    if (ok != pdPASS) {
        s_uart_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool ld2450_is_running(void)
{
    return s_uart_task != NULL;
}

void ld2450_rx_pause(void)
{
    if (!s_uart_task) return;
    s_rx_pause_requested = true;
    // Wait for the RX task to actually pause (up to 200ms for current read to finish)
    xSemaphoreTake(s_rx_paused_sem, pdMS_TO_TICKS(200));
}

void ld2450_rx_resume(void)
{
    if (!s_uart_task) return;
    s_rx_pause_requested = false;
    xTaskNotifyGive(s_uart_task);  // wake the RX task
}

esp_err_t ld2450_get_runtime_cfg(ld2450_runtime_cfg_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ld2450_get_state(ld2450_state_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ld2450_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    s_cfg.enabled = enabled;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ld2450_set_tracking_mode(ld2450_tracking_mode_t mode)
{
    if (mode != LD2450_TRACK_MULTI && mode != LD2450_TRACK_SINGLE) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    s_cfg.mode = mode;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ld2450_set_publish_coords(bool enable)
{
    portENTER_CRITICAL(&s_lock);
    s_cfg.publish_coords = enable;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ld2450_get_zones(ld2450_zone_t *out, size_t count)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (count < LD2450_ZONE_COUNT) return ESP_ERR_INVALID_SIZE;
    portENTER_CRITICAL(&s_lock);
    memcpy(out, s_zones, sizeof(s_zones));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t ld2450_set_zones(const ld2450_zone_t *zones, size_t count)
{
    if (!zones) return ESP_ERR_INVALID_ARG;
    if (count != LD2450_ZONE_COUNT) return ESP_ERR_INVALID_SIZE;

    for (size_t i = 0; i < LD2450_ZONE_COUNT; i++) {
        if (!zone_vertices_sane(&zones[i])) return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    memcpy(s_zones, zones, sizeof(s_zones));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

uart_port_t ld2450_get_uart_port(void)
{
    return s_uart_num;
}

esp_err_t ld2450_set_zone(size_t zone_index, const ld2450_zone_t *zone)
{
    if (!zone) return ESP_ERR_INVALID_ARG;
    if (zone_index >= LD2450_ZONE_COUNT) return ESP_ERR_INVALID_ARG;
    if (!zone_vertices_sane(zone)) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_lock);
    s_zones[zone_index] = *zone;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
