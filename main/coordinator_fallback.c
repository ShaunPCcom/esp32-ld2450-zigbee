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
    bool     occupied;                /* current occupancy state for this EP (from normal SM) */
    bool     awaiting_ack;            /* ACK window is open (alarm scheduled) */
    bool     soft_fallback_active;    /* this EP is in soft fallback (transient) */
    bool     fallback_session_active; /* entered occupancy under hard fallback */
    bool     fallback_occupied;       /* fallback SM occupancy (independent cooldown) */
    uint8_t  fb_cooldown_gen;         /* generation counter for stale timer invalidation */
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
/*  Occupancy report retry queue                                        */
/* ================================================================== */

#define OCC_RETRY_QUEUE_SIZE   16
#define OCC_MAX_RETRIES        3
#define OCC_KEEPALIVE_MS       300000  /* 5 minutes */

static const uint32_t k_retry_delay_ms[OCC_MAX_RETRIES] = {250, 500, 1000};

typedef struct {
    uint8_t  ep;
    uint8_t  attempts;            /* retries fired so far (0 before first send) */
    uint8_t  value;               /* occupancy value being reported */
    uint8_t  next_value;          /* supersede value pending in-flight completion */
    uint8_t  in_use            : 1;
    uint8_t  in_flight         : 1;
    uint8_t  pending_supersede : 1;
} occ_slot_t;

static occ_slot_t s_q[OCC_RETRY_QUEUE_SIZE];
static uint8_t    s_ka_gen = 0;  /* keep-alive generation; invalidates stale alarms */

/* ================================================================== */
/*  Forward declarations                                                */
/* ================================================================== */

static void hard_timeout_cb(uint8_t param);
static void fallback_cooldown_cb(uint8_t param);
static void heartbeat_timeout_cb(uint8_t param);
static void send_onoff_via_binding(uint8_t endpoint, bool on);
static void enter_fallback_mode(void);
static void start_heartbeat_watchdog(void);
static void cancel_heartbeat_watchdog(void);
static void enter_soft_fallback_for_ep(uint8_t ep_idx);
static void q_send_now(occ_slot_t *slot);
static void q_on_send_status(uint8_t ep, bool ok);
static void q_enqueue_or_coalesce(uint8_t ep, uint8_t value);
static void q_retry_alarm_cb(uint8_t param);
static void keepalive_alarm_cb(uint8_t param);

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

static void reconcile_fallback_to_normal(void)
{
    for (int i = 0; i < 11; i++) {
        uint8_t endpoint = i + 1;
        uint16_t cluster = ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING;
        uint16_t attr    = ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID;
        uint8_t  val     = s_ep[i].fallback_occupied ? 1 : 0;

        if (i == 0) {
            esp_zb_zcl_set_attribute_val(ZB_EP_MAIN, cluster,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, &val, false);
        } else {
            esp_zb_zcl_set_attribute_val(ZB_EP_ZONE(i - 1), cluster,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, &val, false);
        }

        /* Cancel pending fallback cooldown timer */
        uint8_t cancel_param = (uint8_t)((i << 4) | (s_ep[i].fb_cooldown_gen & 0x0F));
        esp_zb_scheduler_alarm_cancel(fallback_cooldown_cb, cancel_param);

        /* Sync fallback_occupied to current normal SM state */
        s_ep[i].fallback_occupied = s_ep[i].occupied;

        ESP_LOGI(TAG, "ep%u: reconciled to Z2M (occ=%d)", endpoint, val);
    }
}

/* ================================================================== */
/*  Soft fallback entry (called by retry queue on exhaustion)           */
/* ================================================================== */

static void enter_soft_fallback_for_ep(uint8_t ep_idx)
{
    if (!s_fallback_enabled || s_fallback_mode) return;

    uint8_t endpoint = ep_idx + 1;
    bool    fb_occ   = s_ep[ep_idx].fallback_occupied;
    bool    already  = s_ep[ep_idx].soft_fallback_active;

    s_ep[ep_idx].soft_fallback_active = true;

    if (!already) {
        s_soft_fault_count++;
        set_soft_fault_attr(s_soft_fault_count);
        ESP_LOGW(TAG, "ep%u: soft fallback #%u (retry exhausted, fb_occ=%d)",
                 endpoint, s_soft_fault_count, fb_occ);
    } else {
        ESP_LOGD(TAG, "ep%u: retry exhausted (already in soft fallback)", endpoint);
        return;  /* hard timeout already armed */
    }

    if (fb_occ) {
        send_onoff_via_binding(endpoint, true);
    } else {
        send_onoff_via_binding(endpoint, false);
    }

    if (!s_hard_timeout_pending) {
        s_hard_timeout_gen++;
        esp_zb_scheduler_alarm(hard_timeout_cb, s_hard_timeout_gen,
                               (uint32_t)s_hard_timeout_sec * 1000);
        s_hard_timeout_pending = true;
        ESP_LOGI(TAG, "Hard timeout scheduled in %us", s_hard_timeout_sec);
    }
}

/* ================================================================== */
/*  Occupancy report retry queue implementation                        */
/* ================================================================== */

static void q_send_now(occ_slot_t *slot)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.src_endpoint = slot->ep;
    cmd.address_mode   = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
    cmd.clusterID      = ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING;
    cmd.direction      = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.dis_default_resp = 1;
    cmd.attributeID    = ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID;

    slot->in_flight = 1;
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err != ESP_OK) {
        slot->in_flight = 0;
        ESP_LOGW(TAG, "ep%u: report_attr_cmd_req failed (%d), scheduling retry", slot->ep, err);
        q_on_send_status(slot->ep, false);
        return;
    }
    ESP_LOGD(TAG, "ep%u: occ report sent (val=%u attempt=%u)", slot->ep, slot->value, slot->attempts);
}

static void q_on_send_status(uint8_t ep, bool ok)
{
    occ_slot_t *slot = NULL;
    for (int i = 0; i < OCC_RETRY_QUEUE_SIZE; i++) {
        if (s_q[i].in_use && s_q[i].in_flight && s_q[i].ep == ep) {
            slot = &s_q[i];
            break;
        }
    }
    if (!slot) return;  /* no in-flight slot for this EP (stale callback) */

    slot->in_flight = 0;

    if (ok) {
        ESP_LOGD(TAG, "ep%u: occ report ACK (attempts=%u)", ep, slot->attempts);
        if (slot->pending_supersede) {
            uint8_t next_val = slot->next_value;
            slot->in_use = 0;
            q_enqueue_or_coalesce(ep, next_val);
        } else {
            slot->in_use = 0;
        }
        return;
    }

    /* Send failed — retry or exhaust */
    slot->attempts++;
    if (slot->attempts > OCC_MAX_RETRIES) {
        ESP_LOGW(TAG, "ep%u: occ report retry exhausted (val=%u)", ep, slot->value);
        uint8_t ep_idx = ep - 1;
        if (slot->pending_supersede) {
            uint8_t next_val = slot->next_value;
            slot->in_use = 0;
            q_enqueue_or_coalesce(ep, next_val);
        } else {
            slot->in_use = 0;
        }
        enter_soft_fallback_for_ep(ep_idx);
        return;
    }

    uint32_t delay = k_retry_delay_ms[slot->attempts - 1];
    ESP_LOGD(TAG, "ep%u: occ retry %u in %ums", ep, slot->attempts, delay);
    esp_zb_scheduler_alarm(q_retry_alarm_cb, (uint8_t)(ep - 1), delay);
}

static void q_enqueue_or_coalesce(uint8_t ep, uint8_t value)
{
    /* Update existing slot for this EP if one exists */
    for (int i = 0; i < OCC_RETRY_QUEUE_SIZE; i++) {
        if (!s_q[i].in_use || s_q[i].ep != ep) continue;

        if (!s_q[i].in_flight) {
            s_q[i].value    = value;
            s_q[i].attempts = 0;
            ESP_LOGD(TAG, "ep%u: coalesced occ report (val=%u)", ep, value);
            return;
        }
        s_q[i].pending_supersede = 1;
        s_q[i].next_value        = value;
        ESP_LOGD(TAG, "ep%u: pending supersede after in-flight (val=%u)", ep, value);
        return;
    }

    /* Allocate new slot */
    for (int i = 0; i < OCC_RETRY_QUEUE_SIZE; i++) {
        if (!s_q[i].in_use) {
            s_q[i] = (occ_slot_t){ .ep = ep, .value = value, .in_use = 1 };
            q_send_now(&s_q[i]);
            return;
        }
    }

    /* Queue full: drop oldest non-in-flight slot */
    ESP_LOGW(TAG, "ep%u: retry queue full, dropping oldest pending slot", ep);
    for (int i = 0; i < OCC_RETRY_QUEUE_SIZE; i++) {
        if (s_q[i].in_use && !s_q[i].in_flight) {
            s_q[i] = (occ_slot_t){ .ep = ep, .value = value, .in_use = 1 };
            q_send_now(&s_q[i]);
            return;
        }
    }
    ESP_LOGE(TAG, "ep%u: all queue slots in flight, report dropped", ep);
}

static void q_retry_alarm_cb(uint8_t param)
{
    uint8_t ep_idx = param;
    if (ep_idx > 10) return;
    uint8_t ep = ep_idx + 1;

    for (int i = 0; i < OCC_RETRY_QUEUE_SIZE; i++) {
        if (s_q[i].in_use && !s_q[i].in_flight && s_q[i].ep == ep) {
            q_send_now(&s_q[i]);
            return;
        }
    }
}

static void keepalive_alarm_cb(uint8_t param)
{
    if (param != s_ka_gen) return;  /* stale alarm */

    ESP_LOGD(TAG, "Keep-alive: enqueuing all 11 EP occupancy reports");
    for (int i = 0; i < 11; i++) {
        q_enqueue_or_coalesce((uint8_t)(i + 1), s_ep[i].occupied ? 1 : 0);
    }
    esp_zb_scheduler_alarm(keepalive_alarm_cb, s_ka_gen, OCC_KEEPALIVE_MS);
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

    /* Route to retry queue for any in-flight occupancy report from this EP */
    for (int i = 0; i < OCC_RETRY_QUEUE_SIZE; i++) {
        if (s_q[i].in_use && s_q[i].in_flight && s_q[i].ep == src_ep) {
            q_on_send_status(src_ep, success);
            break;
        }
    }

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

            /* Reconcile: push fallback SM state to Z2M for earliest recovery */
            reconcile_fallback_to_normal();
            ESP_LOGI(TAG, "Coordinator ACK -- soft fallbacks cleared, state reconciled to Z2M");
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
        ESP_LOGW(TAG, "ep%u: send failed (status=%d)", src_ep, msg.status);
    }
}

/* ================================================================== */
/*  ACK timeout callback                                                */
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
/*  Fallback cooldown timer callback                                    */
/* ================================================================== */

static void fallback_cooldown_cb(uint8_t param)
{
    uint8_t ep_idx = (param >> 4) & 0x0F;
    uint8_t gen    = param & 0x0F;

    if (ep_idx > 10) return;
    uint8_t endpoint = ep_idx + 1;

    if ((s_ep[ep_idx].fb_cooldown_gen & 0x0F) != gen) {
        ESP_LOGD(TAG, "ep%u: stale fallback cooldown (gen mismatch), ignoring", endpoint);
        return;
    }

    s_ep[ep_idx].fallback_occupied = false;
    ESP_LOGI(TAG, "ep%u: fallback cooldown expired, fallback_occupied=0", endpoint);

    if (s_fallback_mode || s_ep[ep_idx].soft_fallback_active) {
        send_onoff_via_binding(endpoint, false);
        ESP_LOGI(TAG, "ep%u: sent Off via binding (fallback cooldown expired)", endpoint);
    }
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
        if (s_hard_timeout_pending) {
            esp_zb_scheduler_alarm_cancel(hard_timeout_cb, s_hard_timeout_gen);
            s_hard_timeout_pending = false;
        }
        for (int i = 0; i < 11; i++) {
            s_ep[i].soft_fallback_active = false;
            s_ep[i].awaiting_ack = false;
            uint8_t cancel_param = (uint8_t)((i << 4) | (s_ep[i].fb_cooldown_gen & 0x0F));
            esp_zb_scheduler_alarm_cancel(fallback_cooldown_cb, cancel_param);
        }
        if (s_soft_fault_count > 0) {
            s_soft_fault_count = 0;
            set_soft_fault_attr(0);
        }
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
    memset(s_q, 0, sizeof(s_q));

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

    /* ---- Fallback SM: always track, independent of normal SM ---- */
    if (occupied) {
        uint8_t cancel_param = (uint8_t)((ep_idx << 4) | (s_ep[ep_idx].fb_cooldown_gen & 0x0F));
        esp_zb_scheduler_alarm_cancel(fallback_cooldown_cb, cancel_param);
        s_ep[ep_idx].fallback_occupied = true;
    } else {
        nvs_config_t cfg;
        nvs_config_get(&cfg);
        uint16_t cooldown_sec = cfg.fallback_cooldown_sec[ep_idx];

        s_ep[ep_idx].fb_cooldown_gen++;
        uint8_t gen   = s_ep[ep_idx].fb_cooldown_gen;
        uint8_t fb_param = (uint8_t)((ep_idx << 4) | (gen & 0x0F));

        if (cooldown_sec == 0) {
            s_ep[ep_idx].fallback_occupied = false;
            ESP_LOGD(TAG, "ep%u: fallback cooldown=0, fallback_occupied=0 immediately", endpoint);
        } else {
            esp_zb_scheduler_alarm(fallback_cooldown_cb, fb_param,
                                   (uint32_t)cooldown_sec * 1000);
            ESP_LOGD(TAG, "ep%u: fallback cooldown started (%us), fallback_occupied stays 1",
                     endpoint, cooldown_sec);
        }
    }

    /* ---- Dispatch: hard fallback path ---- */
    if (s_fallback_mode) {
        s_ep[ep_idx].fallback_session_active = true;

        if (s_ep[ep_idx].fallback_occupied) {
            send_onoff_via_binding(endpoint, true);
            ESP_LOGI(TAG, "ep%u: sent On via binding (hard fallback)", endpoint);
        } else {
            send_onoff_via_binding(endpoint, false);
            ESP_LOGI(TAG, "ep%u: sent Off via binding (hard fallback)", endpoint);
        }
        return;
    }

    /* ---- Dispatch: soft fallback path (skip probing) ---- */
    if (s_ep[ep_idx].soft_fallback_active) {
        if (s_ep[ep_idx].fallback_occupied) {
            send_onoff_via_binding(endpoint, true);
            ESP_LOGI(TAG, "ep%u: sent On via binding (soft fallback, occ change)", endpoint);
        } else {
            send_onoff_via_binding(endpoint, false);
            ESP_LOGI(TAG, "ep%u: sent Off via binding (soft fallback, occ change)", endpoint);
        }
        return;
    }

    /* Normal mode: occupancy reporting is handled by coordinator_fallback_report_occupancy
     * (explicit report_attr_cmd_req with retry queue). Nothing to do here. */
}

void coordinator_fallback_report_occupancy(uint8_t ep, bool occupied)
{
    if (ep < 1 || ep > 11) return;
    q_enqueue_or_coalesce(ep, occupied ? 1 : 0);
}

void coordinator_fallback_start_keepalive(void)
{
    s_ka_gen++;
    esp_zb_scheduler_alarm(keepalive_alarm_cb, s_ka_gen, OCC_KEEPALIVE_MS);
    ESP_LOGI(TAG, "Occupancy keep-alive started (period=%us)", OCC_KEEPALIVE_MS / 1000);
}

bool coordinator_fallback_is_active(void)
{
    return s_fallback_mode;
}

bool coordinator_fallback_ep_session_active(uint8_t ep_idx)
{
    if (ep_idx > 10) return false;
    return s_ep[ep_idx].fallback_session_active;
}

void coordinator_fallback_clear(void)
{
    ESP_LOGI(TAG, "Hard fallback cleared by HA (fallback_mode=0)");

    /* Reconcile: push fallback SM state to Z2M for earliest recovery */
    reconcile_fallback_to_normal();

    s_fallback_mode        = false;
    s_fallback_reported    = false;
    s_coordinator_reachable = true;

    if (s_hard_timeout_pending) {
        esp_zb_scheduler_alarm_cancel(hard_timeout_cb, s_hard_timeout_gen);
        s_hard_timeout_pending = false;
    }

    for (int i = 0; i < 11; i++) {
        s_ep[i].fallback_session_active = false;
        s_ep[i].soft_fallback_active    = false;
        s_ep[i].awaiting_ack            = false;
    }

    s_soft_fault_count = 0;
    set_soft_fault_attr(0);

    nvs_config_save_fallback_mode(0);

    uint8_t val = 0;
    esp_zb_zcl_set_attribute_val(ZB_EP_MAIN,
        ZB_CLUSTER_LD2450_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZB_ATTR_FALLBACK_MODE,
        &val, false);

    if (s_heartbeat_enabled) {
        cancel_heartbeat_watchdog();
        start_heartbeat_watchdog();
    }

    ESP_LOGI(TAG, "Hard fallback cleared -- state reconciled, normal tracking resumed");
}

void coordinator_fallback_set(void)
{
    ESP_LOGI(TAG, "Hard fallback manually set (HA/CLI)");
    if (!s_fallback_mode) {
        enter_fallback_mode();
    }
}
