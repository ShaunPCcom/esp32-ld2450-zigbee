// SPDX-License-Identifier: MIT
/**
 * @file zigbee_signal_handlers.h
 * @brief LD2450-specific Zigbee lifecycle hook registration.
 *
 * The common network lifecycle handler (esp_zb_app_signal_handler,
 * zigbee_factory_reset, zigbee_full_factory_reset, reboot_cb, etc.) is
 * provided by the shared zigbee_signal_handler component. This header
 * re-exports those declarations and adds the LD2450 setup function.
 */

#pragma once

#include "zigbee_signal_handler.h"  /* re-exports common API */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register LD2450-specific Zigbee lifecycle hooks.
 *
 * Must be called before the Zigbee stack starts (i.e. before zigbee_init()).
 * Registers on_joined and on_unhandled_signal callbacks and the NVS namespace
 * for full factory reset.
 */
void zigbee_signal_handlers_setup(void);

#ifdef __cplusplus
}
#endif
