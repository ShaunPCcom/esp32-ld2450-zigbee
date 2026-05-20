// SPDX-License-Identifier: MIT
#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdint.h>

/**
 * @file config_api.h
 * @brief Shared configuration API used by both Zigbee attribute handler and REST API.
 *
 * Each function validates its input, persists to NVS, and applies the side effect
 * (sensor command, runtime state change, etc.). Both the Zigbee attr handler and
 * the HTTP REST API call these functions — they are peers, neither owns config.
 *
 * Zone coordinate functions return ESP_ERR_INVALID_ARG when the CSV pair count
 * does not match the stored vertex_count.  The caller is responsible for any
 * protocol-level revert (e.g. writing back the old ZCL attribute value).
 */

/* ---- Sensor hardware config ---- */
esp_err_t config_api_set_max_distance(uint16_t mm);
esp_err_t config_api_set_angle_left(uint8_t deg);
esp_err_t config_api_set_angle_right(uint8_t deg);
esp_err_t config_api_set_tracking_mode(uint8_t mode);
esp_err_t config_api_set_publish_coords(uint8_t enabled);

/* ---- Occupancy timing (ep_idx: 0=main EP, 1-10=zones) ---- */
esp_err_t config_api_set_occupancy_cooldown(uint8_t ep_idx, uint16_t sec);
esp_err_t config_api_set_occupancy_delay(uint8_t ep_idx, uint16_t ms);

/* ---- Coordinator fallback ---- */
esp_err_t config_api_set_fallback_mode(uint8_t mode);
esp_err_t config_api_set_fallback_cooldown(uint8_t ep_idx, uint16_t sec);
esp_err_t config_api_set_fallback_enable(uint8_t enable);
esp_err_t config_api_set_hard_timeout(uint8_t sec);
esp_err_t config_api_set_ack_timeout(uint16_t ms);

/* ---- Heartbeat watchdog ---- */
esp_err_t config_api_set_heartbeat_enable(uint8_t enable);
esp_err_t config_api_set_heartbeat_interval(uint16_t sec);
esp_err_t config_api_heartbeat(void);

/* ---- Zone geometry (zone_idx: 0-9) ---- */

/**
 * Set zone vertex count.  If vc < 3, the zone is disabled and coords are zeroed.
 * When vc >= 3 with no coords yet, only the in-memory cache is updated; the
 * subsequent config_api_set_zone_coords call will persist the full zone to NVS.
 */
esp_err_t config_api_set_zone_vertex_count(uint8_t zone_idx, uint8_t vc);

/**
 * Set zone coords from a CSV string (e.g. "100,200;300,400;500,600").
 * Returns ESP_ERR_INVALID_ARG if the pair count does not match the stored
 * vertex_count for this zone.  Caller must handle protocol-level revert.
 */
esp_err_t config_api_set_zone_coords(uint8_t zone_idx, const char *csv);

/* ---- Bluetooth state ---- */
esp_err_t config_api_set_bt_disabled(uint8_t disabled);

/* ---- Read-all for REST/WebSocket and portable backup ---- */

/**
 * Serialize the entire device configuration to a newly allocated cJSON object.
 * Caller must call cJSON_Delete(*out) when done.
 * Returns ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t config_api_get_all(cJSON **out);

/**
 * Apply a full configuration JSON object (as produced by config_api_get_all).
 * Validates and applies all recognised keys; unknown keys are silently ignored.
 * Used by both handle_post_config and the portable backup import path.
 */
esp_err_t config_api_apply_all(const cJSON *root);
