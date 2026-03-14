// SPDX-License-Identifier: MIT
#pragma once

#include "esp_err.h"
#include "ld2450.h"
#include "ld2450_zone.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * Persistent configuration stored in NVS.
 * All fields have sensible defaults if NVS is empty.
 */
typedef struct {
    /* Software config */
    uint8_t tracking_mode;      /* 0=multi, 1=single */
    uint8_t publish_coords;     /* 0=off, 1=on */

    /* Sensor hardware config (applied via LD2450 commands) */
    uint16_t max_distance_mm;   /* 0-6000 */
    uint8_t angle_left_deg;     /* 0-90 */
    uint8_t angle_right_deg;    /* 0-90 */
    uint8_t bt_disabled;        /* 0=BT on, 1=BT off */

    /* Zones */
    ld2450_zone_t zones[10];

    /* Occupancy reporting */
    uint16_t occupancy_cooldown_sec[11]; /* 0-300 seconds per endpoint: [0]=main, [1-10]=zones */
    uint16_t occupancy_delay_ms[11];     /* 0-65535 milliseconds per endpoint: [0]=main, [1-10]=zones */
} nvs_config_t;

/** Initialize NVS config module and load saved config (or defaults). */
esp_err_t nvs_config_init(void);

/** Get a copy of the current loaded config. */
esp_err_t nvs_config_get(nvs_config_t *out);

/* Per-field save functions. Each updates the in-memory copy and writes to NVS. */
esp_err_t nvs_config_save_tracking_mode(uint8_t mode);
esp_err_t nvs_config_save_publish_coords(uint8_t enabled);
esp_err_t nvs_config_save_max_distance(uint16_t mm);
esp_err_t nvs_config_save_angle_left(uint8_t deg);
esp_err_t nvs_config_save_angle_right(uint8_t deg);
esp_err_t nvs_config_save_bt_disabled(uint8_t disabled);
esp_err_t nvs_config_save_zone(uint8_t zone_index, const ld2450_zone_t *zone);

/** Update the in-memory zone cache without writing to NVS flash.
 *  Used during the two-phase write protocol: vertex_count arrives before coords.
 *  Call this when vertex_count >= 3 but coords are not yet present.
 *  The subsequent coords write will call nvs_config_save_zone to persist. */
void nvs_config_update_zone_cache(uint8_t zone_index, const ld2450_zone_t *zone);
esp_err_t nvs_config_save_occupancy_cooldown(uint8_t endpoint_index, uint16_t sec);
esp_err_t nvs_config_save_occupancy_delay(uint8_t endpoint_index, uint16_t ms);
