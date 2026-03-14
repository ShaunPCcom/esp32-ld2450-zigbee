// SPDX-License-Identifier: MIT
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start Zigbee stack
 *
 * Creates FreeRTOS task that initializes platform, registers endpoints,
 * and starts the Zigbee stack main loop.
 */
void zigbee_init(void);

/**
 * @brief Sync zone config attributes on EP1 from NVS values.
 *
 * Call after the Zigbee stack has started (e.g. from the on_startup
 * signal handler). Populates vertex_count, coords CSV, cooldown, and
 * delay attributes for all 10 zones.
 */
void zigbee_sync_zone_attrs_from_nvs(void);

#ifdef __cplusplus
}
#endif
