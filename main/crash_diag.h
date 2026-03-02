// SPDX-License-Identifier: MIT
#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @file crash_diag.h
 * @brief Crash diagnostics and telemetry for remote debugging
 *
 * Collects boot count, reset reason, uptime, and heap statistics to diagnose
 * random reboots without physical access. Data stored in RTC memory (survives
 * most resets) and NVS (survives power loss).
 *
 * Exposed as Zigbee attributes on cluster 0xFC00 for remote monitoring via
 * Home Assistant.
 */

/**
 * Diagnostic data collected at boot
 */
typedef struct {
    uint32_t boot_count;        /**< Monotonic boot counter (increments on every reset) */
    uint8_t  reset_reason;      /**< Last reset cause (from esp_reset_reason()) */
    uint32_t last_uptime_sec;   /**< Uptime in seconds before last reset (0 if unknown) */
    uint32_t min_free_heap;     /**< Minimum free heap size since last boot */
} crash_diag_data_t;

/**
 * Initialize crash diagnostics system.
 *
 * Must be called early in app_main(), after nvs_flash_init() but before
 * any other subsystems that might trigger resets.
 *
 * Actions performed:
 * - Reads boot_count from NVS, increments it, saves back
 * - Reads reset_reason from RTC memory (or queries ESP-IDF)
 * - Reads last_uptime from RTC memory
 * - Initializes heap monitoring
 * - Stores current data in RTC memory for next boot
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t crash_diag_init(void);

/**
 * Get current diagnostic data.
 *
 * @param[out] data  Pointer to structure to fill with diagnostic data
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if data is NULL
 */
esp_err_t crash_diag_get_data(crash_diag_data_t *data);

/**
 * Get human-readable string for reset reason code.
 *
 * @param reason  Reset reason code from esp_reset_reason()
 * @return Constant string describing the reset reason
 */
const char *crash_diag_reset_reason_str(uint8_t reason);

/**
 * Update uptime in RTC memory.
 *
 * Should be called periodically (e.g., every minute) to keep uptime current
 * in case of unexpected reset. NOT required to be called - if not called,
 * last_uptime will be 0 on next boot.
 *
 * @param uptime_sec  Current uptime in seconds
 */
void crash_diag_update_uptime(uint32_t uptime_sec);
