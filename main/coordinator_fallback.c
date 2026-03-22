// SPDX-License-Identifier: MIT
#include "coordinator_fallback.h"

#include <string.h>

#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_core.h"
#include "aps/esp_zigbee_aps.h"

#include "nvs_config.h"
#include "zigbee_defs.h"

static const char *TAG = "fallback";

/* ================================================================== */
/*  Per-endpoint state                                                  */
/* ================================================================== */

typedef struct {
    bool     occupied;                /* current occupancy state for this EP */
    bool     awaiting_ack;            /* ACK window is open (alarm scheduled) */
    bool     soft_fallback_active;    /* this EP is in soft fallback (transient) */
    bool     fallback_session_active; /* entered occupancy under hard fallback */
} fallback_ep_state_t;

/* s_ep[0] = EP1 (main), s_ep[1-10] = EP2-11 (zones) */
static fallback_ep_state_t s_ep[11];

/* ================================================================== */
/*  Global fallback state                                               */
/* ================================================================== */

static bool s_fallback_mode        = false;  /* sticky hard fallback, NVS-backed */
static bool s_coordinator_reachable = true;  /* optimistic: assume reachable until miss */
static bool s_fallback_reported    = false;  /* have we reported flag=1 to coordinator? */

/* ================================================================== */
/*  Soft/hard two-tier state                                            */
/* ================================================================== */

static bool     s_fallback_enabled     = false;  /* global enable gate (NVS-backed) */
static uint8_t  s_soft_fault_count     = 0;       /* firmware-only; cleared on coordinator ACK */
static bool     s_hard_timeout_pending = false;
static uint8_t  s_hard_timeout_sec     = 10;      /* seconds until soft->hard escalation */
static uint8_t  s_hard_timeout_gen     = 0;       /* stale timer invalidation */
static uint16_t s_ack_timeout_ms       = 2000;    /* APS ACK timeout (configurable) */

/* ================================================================== */
/*  Software watchdog (heartbeat) state                                */
/* ================================================================== */

static bool     s_heartbeat_enabled      = false;
static uint16_t s_heartbeat_interval_sec = 120;
static uint8_t  s_heartbeat_gen          = 0;  /* generation counter -- invalidates stale timers */

/* ================================================================== */
/*  Forward declarations                                                */
/* ================================================================== */

static void ack_timeout_cb(uint8_t param);
static void hard_timeout_cb(uint8_t param);
static void heartbeat_timeout_cb(uint8_t param);
static void send_onoff_via_binding(uint8_t endpoint, bool on);
static void enter_fallback_mode(void);
static void start_heartbeat_watchdog(void);
static void cancel_heartbeat_watchdog(void);

/* ================================================================== */
/*  ZCL attribute helpers                                               */
/* ================================================================== */

static void set_soft_fault_attr(uint8_t count)
{
    uint8_t val = count;
    esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
        ZB_CLUSTER_LD2450_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZB_ATTR_SOFT_FAULT,
        &val, false);
}

/* ================================================================== */
/*  Send-status callback (APS ACK tracking)                            */
/* ================================================================== */

/*
 * Probe is always sent from EP1 via custom_cluster_cmd_req() which gives us
 * a trackable APS ACK here.  On/Off binding commands are filtered out by the
 * short_addr != 0x0000 check.
 */
static void send_status_cb(esp_zb_zcl_command_send_status_message_t msg)
{
    /* Only care about messages to the coordinator (short addr 0x0000) */
    if (msg.dst_addr.addr_type != 0 /* ESP_ZB_ZCL_ADDR_TYPE_SHORT */
            || msg.dst_addr.u.short_addr != 0x0000) {
        return;
    }

    uint8_t src_ep  = msg.src_endpoint;
    bool    success = (msg.status == ESP_OK);

    ESP_LOGI(TAG, "send_status: ep=%u dst_ep=%u status=%s",
             src_ep, msg.dst_endpoint, success ? "OK" : "FAIL");

    if (src_ep < 1 || src_ep > 11) return;

    if (success) {
        /* Coordinator ACKed -- clear awaiting_ack for ALL EPs */
        s_coordinator_reachable = true;
        bool any_cleared = false;
        for (int i = 0; i < 11; i++) {
            if (s_ep[i].awaiting_ack) {
                s_ep[i].awaiting_ack = false;
                any_cleared = true;
            }
        }
        if (any_cleared) {
            ESP_LOGI(TAG, "coordinator ACK (ep%u probe) -- cleared all pending acks", src_ep);
        }

        /* Clear any soft fallbacks -- coordinator is alive, HA reconciles lights */
        bool any_soft = false;
        for (int i = 0; i < 11; i++) {
            if (s_ep[i].soft_fallback_active) { any_soft = true; break; }
        }
        if (any_soft) {
            for (int i = 0; i < 11; i++) {
                s_ep[i].soft_fallback_active = false;
            }
            if (s_hard_timeout_pending) {
                esp_zb_scheduler_alarm_cancel(hard_timeout_cb, s_hard_timeout_gen);
                s_hard_timeout_pending = false;
            }
            s_soft_fault_count = 0;
            set_soft_fault_attr(0);
            ESP_LOGI(TAG, "Coordinator ACK -- all soft fallbacks cleared (HA reconciles)");
            /* Do NOT send Off commands -- HA reconciles via automations */
        }

        /* If in hard fallback and not yet reported, send the fallback flag now */
        if (s_fallback_mode && !s_fallback_reported) {
            uint8_t val = 1;
            esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
                ZB_CLUSTER_LD2450_CONFIG,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ZB_ATTR_FALLBACK_MODE,
                &val, false);
            s_fallback_reported = true;
            ESP_LOGI(TAG, "Coordinator reachable -- reported fallback_mode=1");
        }
    } else {
        ESP_LOGW(TAG, "ep%u: send failed (status=%d) -- ack_timeout_cb will fire", src_ep, msg.status);
    }
}

/* ================================================================== */
/*  ACK timeout callback                                                */
/* ================================================================== */

static void ack_timeout_cb(uint8_t param)
{
    /* Param: (endpoint << 1) | (occupied ? 1 : 0) */
    uint8_t endpoint = param >> 1;
    bool    occupied = (param & 1) != 0;

    if (endpoint < 1 || endpoint > 11) return;
    uint8_t ep_idx = endpoint - 1;

    /* If ACK arrived already, nothing to do */
    if (!s_ep[ep_idx].awaiting_ack) {
        ESP_LOGI(TAG, "ep%u: ACK arrived before timeout, no fallback", endpoint);
        return;
    }

    s_ep[ep_idx].awaiting_ack = false;
    s_coordinator_reachable   = false;

    /* ---- Hard fallback path: already in hard fallback, dispatch directly ---- */
    if (s_fallback_mode) {
        ESP_LOGW(TAG, "ep%u: ACK timeout (hard fallback active, occ=%d)", endpoint, occupied);
        s_ep[ep_idx].fallback_session_active = true;

        if (occupied) {
            send_onoff_via_binding(endpoint, true);
            ESP_LOGI(TAG, "ep%u: sent On via binding (hard fallback)", endpoint);
        } else {
            send_onoff_via_binding(endpoint, false);
            ESP_LOGI(TAG, "ep%u: sent Off via binding (hard fallback)", endpoint);
        }
        return;
    }

    /* ---- Soft fallback path: transient APS timeout ---- */
    bool already_soft = s_ep[ep_idx].soft_fallback_active;
    s_ep[ep_idx].soft_fallback_active = true;

    if (!already_soft) {
        s_soft_fault_count++;
        set_soft_fault_attr(s_soft_fault_count);
        ESP_LOGW(TAG, "ep%u: soft fallback #%u (occ=%d)",
                 endpoint, s_soft_fault_count, occupied);
    } else {
        ESP_LOGW(TAG, "ep%u: additional soft fault (already soft, occ=%d)", endpoint, occupied);
    }

    /* Dispatch On/Off via binding based on current occupancy */
    if (occupied) {
        send_onoff_via_binding(endpoint, true);
        ESP_LOGI(TAG, "ep%u: sent On via binding (soft fallback)", endpoint);
    } else {
        send_onoff_via_binding(endpoint, false);
        ESP_LOGI(TAG, "ep%u: sent Off via binding (soft fallback)", endpoint);
    }

    /* Schedule hard timeout if not already pending */
    if (!s_hard_timeout_pending) {
        s_hard_timeout_gen++;
        esp_zb_scheduler_alarm(hard_timeout_cb, s_hard_timeout_gen,
                               (uint32_t)s_hard_timeout_sec * 1000);
        s_hard_timeout_pending = true;
        ESP_LOGI(TAG, "Hard timeout scheduled in %us (gen=%u)", s_hard_timeout_sec, s_hard_timeout_gen);
    }
}

/* ================================================================== */
/*  Hard timeout callback (soft -> hard escalation)                    */
/* ================================================================== */

static void hard_timeout_cb(uint8_t param)
{
    if (param != s_hard_timeout_gen) return;  /* stale -- cleared by ACK */

    s_hard_timeout_pending = false;

    /* Check if any soft fallbacks are still active */
    bool any_soft = false;
    for (int i = 0; i < 11; i++) {
        if (s_ep[i].soft_fallback_active) { any_soft = true; break; }
    }
    if (!any_soft) {
        ESP_LOGI(TAG, "Hard timeout fired but all soft fallbacks already cleared (stale)");
        return;
    }

    /* Escalate: move soft EP state to hard fallback session state */
    for (int i = 0; i < 11; i++) {
        if (s_ep[i].soft_fallback_active) {
            s_ep[i].soft_fallback_active    = false;
            s_ep[i].fallback_session_active = true;
        }
    }

    ESP_LOGW(TAG, "HARD FALLBACK -- no coordinator response in %us (escalated from soft)",
             s_hard_timeout_sec);
    enter_fallback_mode();
}

/* ================================================================== */
/*  On/Off dispatch via binding                                         */
/* ================================================================== */

static void send_onoff_via_binding(uint8_t endpoint, bool on)
{
    esp_zb_zcl_on_off_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.src_endpoint = endpoint;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
    cmd.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    esp_zb_zcl_on_off_cmd_req(&cmd);
}

/* ================================================================== */
/*  Enter hard fallback mode (internal)                                 */
/* ================================================================== */

static void enter_fallback_mode(void)
{
    s_fallback_mode     = true;
    s_fallback_reported = false;

    /* Save to NVS so reboot resumes in fallback */
    nvs_config_save_fallback_mode(1);

    /* Set ZCL attribute -- will be reported when coordinator reconnects */
    uint8_t val = 1;
    esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
        ZB_CLUSTER_LD2450_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZB_ATTR_FALLBACK_MODE,
        &val, false);

    ESP_LOGW(TAG, "HARD FALLBACK MODE ACTIVE -- On/Off via binding until HA clears");
}

/* ================================================================== */
/*  Software watchdog (heartbeat)                                       */
/* ================================================================== */

static void heartbeat_timeout_cb(uint8_t param)
{
    if (param != s_heartbeat_gen) return;  /* stale -- watchdog was reset */

    ESP_LOGW(TAG, "Heartbeat timeout -- coordinator software offline, entering HARD fallback");

    /* Heartbeat timeout is always a hard fault (software death) */
    if (!s_fallback_mode) {
        enter_fallback_mode();
    }
}

static void start_heartbeat_watchdog(void)
{
    s_heartbeat_gen++;
    uint32_t timeout_ms = (uint32_t)s_heartbeat_interval_sec * 2 * 1000;
    esp_zb_scheduler_alarm(heartbeat_timeout_cb, s_heartbeat_gen, timeout_ms);
    ESP_LOGI(TAG, "Heartbeat watchdog armed: gen=%u timeout=%us",
             s_heartbeat_gen, (unsigned)(s_heartbeat_interval_sec * 2));
}

static void cancel_heartbeat_watchdog(void)
{
    esp_zb_scheduler_alarm_cancel(heartbeat_timeout_cb, s_heartbeat_gen);
}

void coordinator_fallback_heartbeat(void)
{
    if (!s_heartbeat_enabled) return;
    cancel_heartbeat_watchdog();
    start_heartbeat_watchdog();
    ESP_LOGI(TAG, "Heartbeat received -- watchdog reset (gen=%u)", s_heartbeat_gen);
}

void coordinator_fallback_set_heartbeat_enable(uint8_t enable)
{
    s_heartbeat_enabled = (enable != 0);
    nvs_config_save_heartbeat_enable(enable);

    if (s_heartbeat_enabled) {
        start_heartbeat_watchdog();
        ESP_LOGI(TAG, "Heartbeat watchdog enabled (interval=%us, timeout=%us)",
                 s_heartbeat_interval_sec, (unsigned)(s_heartbeat_interval_sec * 2));
    } else {
        cancel_heartbeat_watchdog();
        ESP_LOGI(TAG, "Heartbeat watchdog disabled");
    }
}

void coordinator_fallback_set_heartbeat_interval(uint16_t interval_sec)
{
    if (interval_sec == 0) interval_sec = 120;
    s_heartbeat_interval_sec = interval_sec;
    nvs_config_save_heartbeat_interval(interval_sec);

    if (s_heartbeat_enabled) {
        cancel_heartbeat_watchdog();
        start_heartbeat_watchdog();
    }
    ESP_LOGI(TAG, "Heartbeat interval -> %us (timeout=%us)",
             interval_sec, (unsigned)(interval_sec * 2));
}

/* ================================================================== */
/*  Soft/hard enable control                                            */
/* ================================================================== */

void coordinator_fallback_set_enable(uint8_t enable)
{
    s_fallback_enabled = (enable != 0);
    nvs_config_save_fallback_enable(enable);

    if (!s_fallback_enabled) {
        /* Cancel pending hard timeout and clear all soft fallbacks */
        if (s_hard_timeout_pending) {
            esp_zb_scheduler_alarm_cancel(hard_timeout_cb, s_hard_timeout_gen);
            s_hard_timeout_pending = false;
        }
        for (int i = 0; i < 11; i++) {
            s_ep[i].soft_fallback_active = false;
            s_ep[i].awaiting_ack = false;
        }
        if (s_soft_fault_count > 0) {
            s_soft_fault_count = 0;
            set_soft_fault_attr(0);
        }
        /* Do NOT clear hard fallback -- that requires explicit HA clear */
        ESP_LOGI(TAG, "Fallback disabled -- soft state cleared, hard fallback preserved");
    } else {
        ESP_LOGI(TAG, "Fallback enabled -- monitoring from next occupancy change");
    }
}

void coordinator_fallback_set_hard_timeout(uint8_t sec)
{
    if (sec == 0) sec = 10;
    s_hard_timeout_sec = sec;
    nvs_config_save_hard_timeout_sec(sec);
    ESP_LOGI(TAG, "Hard timeout -> %us", sec);
}

void coordinator_fallback_set_ack_timeout(uint16_t ms)
{
    if (ms < 500) ms = 500;
    s_ack_timeout_ms = ms;
    nvs_config_save_ack_timeout_ms(ms);
    ESP_LOGI(TAG, "ACK timeout -> %ums", ms);
}

uint8_t coordinator_fallback_get_soft_fault_count(void)
{
    return s_soft_fault_count;
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

void coordinator_fallback_init(void)
{
    memset(s_ep, 0, sizeof(s_ep));

    nvs_config_t cfg;
    nvs_config_get(&cfg);

    s_fallback_mode        = (cfg.fallback_mode != 0);
    s_coordinator_reachable = true;
    s_fallback_reported    = false;

    s_fallback_enabled  = (cfg.fallback_enable != 0);
    s_hard_timeout_sec  = cfg.hard_timeout_sec;
    if (s_hard_timeout_sec == 0) s_hard_timeout_sec = 10;
    s_ack_timeout_ms    = cfg.ack_timeout_ms;
    if (s_ack_timeout_ms == 0) s_ack_timeout_ms = 2000;

    s_heartbeat_enabled      = (cfg.heartbeat_enable != 0);
    s_heartbeat_interval_sec = cfg.heartbeat_interval_sec;
    if (s_heartbeat_interval_sec == 0) s_heartbeat_interval_sec = 120;

    if (s_fallback_mode) {
        ESP_LOGW(TAG, "Resuming HARD fallback from NVS");
    }
    if (s_heartbeat_enabled) {
        start_heartbeat_watchdog();
        ESP_LOGI(TAG, "Heartbeat watchdog armed (interval=%us, timeout=%us)",
                 s_heartbeat_interval_sec, (unsigned)(s_heartbeat_interval_sec * 2));
    }

    esp_zb_zcl_command_send_status_handler_register(send_status_cb);

    ESP_LOGI(TAG, "Fallback init: hard=%u enable=%u ack_to=%ums hard_to=%us heartbeat=%u",
             s_fallback_mode, s_fallback_enabled, s_ack_timeout_ms,
             s_hard_timeout_sec, s_heartbeat_enabled);
}

void coordinator_fallback_on_occupancy_change(uint8_t endpoint, bool occupied)
{
    if (endpoint < 1 || endpoint > 11) return;
    uint8_t ep_idx = endpoint - 1;

    s_ep[ep_idx].occupied = occupied;

    /* If already in hard fallback, handle immediately without ACK probing */
    if (s_fallback_mode) {
        s_ep[ep_idx].fallback_session_active = true;

        if (occupied) {
            send_onoff_via_binding(endpoint, true);
            ESP_LOGI(TAG, "ep%u: sent On via binding (hard fallback)", endpoint);
        } else {
            send_onoff_via_binding(endpoint, false);
            ESP_LOGI(TAG, "ep%u: sent Off via binding (hard fallback)", endpoint);
        }
        return;
    }

    /* If soft fallback is active for this EP, dispatch directly (skip probing) */
    if (s_ep[ep_idx].soft_fallback_active) {
        if (occupied) {
            send_onoff_via_binding(endpoint, true);
            ESP_LOGI(TAG, "ep%u: sent On via binding (soft fallback, occ change)", endpoint);
        } else {
            send_onoff_via_binding(endpoint, false);
            ESP_LOGI(TAG, "ep%u: sent Off via binding (soft fallback, occ change)", endpoint);
        }
        return;
    }

    /* Normal mode: gate probing on fallback_enabled */
    if (!s_fallback_enabled) return;

    s_ep[ep_idx].awaiting_ack = true;

    esp_zb_zcl_custom_cluster_cmd_t probe = {0};
    probe.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
    probe.zcl_basic_cmd.dst_endpoint          = 1;
    probe.zcl_basic_cmd.src_endpoint          = ZB_EP_MAIN;
    probe.address_mode    = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    probe.profile_id      = ESP_ZB_AF_HA_PROFILE_ID;
    probe.cluster_id      = ZB_CLUSTER_LD2450_CONFIG;
    probe.direction       = 0;
    probe.dis_default_resp = 0;
    probe.custom_cmd_id   = 0x00;
    probe.data.size       = 0;
    probe.data.value      = NULL;
    esp_zb_zcl_custom_cluster_cmd_req(&probe);
    ESP_LOGI(TAG, "ep%u: probe sent (occ=%d), ack window=%ums",
             endpoint, (int)occupied, (unsigned)s_ack_timeout_ms);

    uint8_t param = (uint8_t)((endpoint << 1) | (occupied ? 1 : 0));
    esp_zb_scheduler_alarm(ack_timeout_cb, param, s_ack_timeout_ms);
}

bool coordinator_fallback_is_active(void)
{
    return s_fallback_mode;
}

bool coordinator_fallback_ep_session_active(uint8_t ep_idx)
{
    if (ep_idx > 10) return false;
    return s_ep[ep_idx].fallback_session_active
        || s_ep[ep_idx].soft_fallback_active
        || s_ep[ep_idx].awaiting_ack;
}

void coordinator_fallback_clear(void)
{
    ESP_LOGI(TAG, "Hard fallback cleared by HA (fallback_mode=0)");

    s_fallback_mode        = false;
    s_fallback_reported    = false;
    s_coordinator_reachable = true;

    /* Cancel pending hard timeout */
    if (s_hard_timeout_pending) {
        esp_zb_scheduler_alarm_cancel(hard_timeout_cb, s_hard_timeout_gen);
        s_hard_timeout_pending = false;
    }

    /* Reset per-EP session state */
    for (int i = 0; i < 11; i++) {
        s_ep[i].fallback_session_active = false;
        s_ep[i].soft_fallback_active    = false;
        s_ep[i].awaiting_ack            = false;
    }

    /* Reset soft fault counter */
    s_soft_fault_count = 0;
    set_soft_fault_attr(0);

    nvs_config_save_fallback_mode(0);

    uint8_t val = 0;
    esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
        ZB_CLUSTER_LD2450_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZB_ATTR_FALLBACK_MODE,
        &val, false);

    /* Do NOT send Off -- HA reconciles */

    if (s_heartbeat_enabled) {
        cancel_heartbeat_watchdog();
        start_heartbeat_watchdog();
    }

    ESP_LOGI(TAG, "Hard fallback cleared -- normal ACK tracking resumed");
}

void coordinator_fallback_set(void)
{
    ESP_LOGI(TAG, "Hard fallback manually set (HA/CLI)");
    if (!s_fallback_mode) {
        enter_fallback_mode();
    }
}
