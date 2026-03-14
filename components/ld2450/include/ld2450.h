// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/uart.h"

#include "ld2450_parser.h"
#include "ld2450_zone.h"

typedef struct {
    uart_port_t uart_num;
    int tx_gpio;
    int rx_gpio;
    int baud_rate;
    int rx_buf_size;
} ld2450_config_t;

esp_err_t ld2450_init(const ld2450_config_t *cfg);

/**
 * Optional helper: returns true if UART task is running.
 */
bool ld2450_is_running(void);

typedef enum {
    LD2450_TRACK_MULTI  = 0,   // evaluate all present targets
    LD2450_TRACK_SINGLE = 1,   // pick one deterministic target
} ld2450_tracking_mode_t;

typedef struct {
    bool enabled;                 // global enable/disable of reporting/eval
    ld2450_tracking_mode_t mode;  // single vs multi
    bool publish_coords;          // "zone edit mode": allow coordinate publishing later
} ld2450_runtime_cfg_t;

typedef struct {
    bool occupied_global;           // any target present (raw from parser)
    uint8_t target_count_raw;       // parser's count
    uint8_t target_count_effective; // after single-target mode policy

    // Selected target (valid if target_count_effective > 0)
    ld2450_target_t selected;

    // Full target array (all 3 slots from parser)
    ld2450_target_t targets[3];

    // Per-zone occupancy (true = occupied)
    bool zone_occupied[10];

    // 10-bit bitmap: bit0=zone1 ... bit9=zone10
    uint16_t zone_bitmap;
} ld2450_state_t;

// Thread-safe: snapshot current config/state
esp_err_t ld2450_get_runtime_cfg(ld2450_runtime_cfg_t *out);
esp_err_t ld2450_get_state(ld2450_state_t *out);

// Thread-safe: update runtime config
esp_err_t ld2450_set_enabled(bool enabled);
esp_err_t ld2450_set_tracking_mode(ld2450_tracking_mode_t mode);
esp_err_t ld2450_set_publish_coords(bool enable);

// Thread-safe zone access (mm internally)
esp_err_t ld2450_get_zones(ld2450_zone_t *out, size_t count);
esp_err_t ld2450_set_zone(size_t zone_index, const ld2450_zone_t *zone);

// Access UART port (for command module)
uart_port_t ld2450_get_uart_port(void);

// Pause/resume the RX task so command module gets exclusive UART access.
// Must be called in pairs. Blocks until the RX task yields.
void ld2450_rx_pause(void);
void ld2450_rx_resume(void);
