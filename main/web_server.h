// SPDX-License-Identifier: MIT
#pragma once

#include "esp_err.h"

/**
 * @file web_server.h
 * @brief Minimal HTTP server for LD2450 web configuration interface (C6 only).
 *
 * Endpoints:
 *   GET  /                    Minimal setup HTML (until LittleFS web UI is ready)
 *   GET  /generate_204        Android captive portal trigger (302 → /)
 *   GET  /hotspot-detect.html iOS captive portal trigger (302 → /)
 *   GET  /ncsi.txt            Windows captive portal trigger (302 → /)
 *   GET  /api/config          JSON of full device config
 *   POST /api/config          Partial config update
 *   GET  /api/status          Firmware version, uptime, heap, WiFi state
 *   POST /api/wifi            Save WiFi credentials (triggers reboot)
 *   POST /api/restart         Reboot device
 *   POST /api/factory-reset   Full factory reset
 */

esp_err_t web_server_start(void);
void      web_server_stop(void);
