// SPDX-License-Identifier: MIT
/**
 * @file zigbee_signal_handlers.c
 * @brief LD2450-specific Zigbee lifecycle callbacks.
 *
 * Registers project-specific hooks with the shared zigbee_signal_handler
 * (from zigbee_core). All common network lifecycle logic lives there.
 */

#include <inttypes.h>

#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "zdo/esp_zigbee_zdo_common.h"

#include "sensor_bridge.h"
#include "zigbee_init.h"
#include "zigbee_signal_handlers.h"

static const char *TAG = "zb_ld2450_hooks";

/* ================================================================== */
/*  Project lifecycle callbacks                                        */
/* ================================================================== */

static void ld2450_on_joined(void)
{
    zigbee_sync_zone_attrs_from_nvs();
    sensor_bridge_start();
}

static void ld2450_on_unhandled_signal(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct ? signal_struct->p_app_signal : NULL;
    if (!p_sg_p) return;

    esp_zb_app_signal_type_t sig = *p_sg_p;
    esp_err_t status = signal_struct->esp_err_status;

    if (sig == ESP_ZB_NLME_STATUS_INDICATION) {
        esp_zb_zdo_signal_nwk_status_indication_params_t *p =
            (esp_zb_zdo_signal_nwk_status_indication_params_t *)
            esp_zb_app_signal_get_params(p_sg_p);
        if (p) {
            ESP_LOGW(TAG, "NWK status indication: code=0x%02x addr=0x%04x",
                     p->status, p->network_addr);
        }
    } else {
        ESP_LOGI(TAG, "ZB signal=0x%08" PRIx32 " status=%s",
                 (uint32_t)sig, esp_err_to_name(status));
    }
}

/* ================================================================== */
/*  Registration                                                       */
/* ================================================================== */

void zigbee_signal_handlers_setup(void)
{
    static const zigbee_signal_hooks_t hooks = {
        .on_joined           = ld2450_on_joined,
        .on_unhandled_signal = ld2450_on_unhandled_signal,
        .nvs_namespace       = "ld2450_cfg",
    };
    zigbee_signal_handler_register(&hooks);
}
