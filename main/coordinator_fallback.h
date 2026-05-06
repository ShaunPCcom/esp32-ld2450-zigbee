// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Coordinator offline fallback module — dual state machine design.
 *
 * Two independent state machines run in parallel:
 *
 * Normal SM (sensor_bridge): Always uses normal cooldown. Reports occupancy
 * to Z2M via ZCL attributes. Feeds every transition to the fallback module.
 *
 * Fallback SM (this module): Maintains its own fallback_occupied per EP with
 * independent fallback cooldown timers. When fallback activates (soft or hard),
 * dispatches On/Off via binding based on fallback_occupied.
 *
 * Reconciliation is one-way: when fallback clears (ACK during soft, or HA
 * clears hard), the fallback SM pushes all EP fallback_occupied states to Z2M
 * attributes, cancels its timers, and lets the normal SM take over. The normal
 * SM then corrects any stale state on its next poll cycle using its own cooldown.
 *
 * Design notes:
 * - "Always armed": if On/Off bindings exist, fallback can activate. To
 *   prevent fallback, remove the On/Off binding — not a firmware config flag.
 * - "Auto re-arm": after HA clears fallback, the device immediately watches
 *   for ACK failures again.
 * - "Earliest recovery": on clear, all EP states are pushed to Z2M so HA
 *   can reconcile immediately.
 */

/** Initialise fallback state, load NVS, register send_status callback. */
void coordinator_fallback_init(void);

/**
 * Called from sensor_bridge after each occupancy attribute change.
 * Starts the ACK timeout window for the given endpoint.
 *
 * @param endpoint  Zigbee endpoint (1=main, 2-11=zones)
 * @param occupied  New occupancy state
 */
void coordinator_fallback_on_occupancy_change(uint8_t endpoint, bool occupied);

/** Returns true if fallback mode is currently active. */
bool coordinator_fallback_is_active(void);

/**
 * Returns true if the given endpoint is in a hard fallback session.
 *
 * @param ep_idx  0=EP1/main, 1-10=EP2-11/zones
 */
bool coordinator_fallback_ep_session_active(uint8_t ep_idx);

/**
 * Clear fallback mode.  Called when HA writes fallback_mode=0.
 * Saves to NVS, resets all per-EP session state, auto re-arms.
 * Does NOT send Off commands to lights — HA reconciles.
 */
void coordinator_fallback_clear(void);

/**
 * Enter fallback mode manually.  Called when HA or CLI writes fallback_mode=1.
 * Normal operation: firmware sets this internally via ACK timeout.
 */
void coordinator_fallback_set(void);

/**
 * Software watchdog (heartbeat) API.
 *
 * When heartbeat_enable=1, the firmware expects periodic writes to the
 * heartbeat attribute from the coordinator application (e.g. an HA automation
 * blueprint).  If no heartbeat arrives within interval×2 seconds, the device
 * enters fallback mode — indicating the coordinator software is down even
 * though the radio hardware (dongle) is still ACKing at the APS layer.
 *
 * This is opt-in.  With heartbeat_enable=0 (default), only hardware failures
 * trigger fallback via the APS ACK probe mechanism.
 */

/** Called when the heartbeat attribute is written.  Resets the watchdog. */
void coordinator_fallback_heartbeat(void);

/** Enable or disable the software watchdog.  Persists to NVS. */
void coordinator_fallback_set_heartbeat_enable(uint8_t enable);

/** Set the expected heartbeat interval.  Watchdog timeout = interval × 2.  Persists to NVS. */
void coordinator_fallback_set_heartbeat_interval(uint16_t interval_sec);

/**
 * Soft/hard two-tier fallback control.
 *
 * fallback_enable=0 (default): probing disabled, pure HA mode.
 *   An existing hard fallback still persists until HA clears it.
 * fallback_enable=1: full soft/hard cycle active.
 *   APS ACK timeout → soft fallback (per-EP, auto-recovering on next ACK).
 *   No ACK within hard_timeout_sec → hard fallback (sticky, NVS-backed).
 */

/** Enable or disable the entire soft/hard fallback system.  Persists to NVS. */
void coordinator_fallback_set_enable(uint8_t enable);

/** Set the hard timeout (seconds from first soft fault → hard fallback).  Persists to NVS. */
void coordinator_fallback_set_hard_timeout(uint8_t sec);

/** Set the APS ACK timeout in ms.  Persists to NVS. */
void coordinator_fallback_set_ack_timeout(uint16_t ms);

/** Return the current soft fault count (read-only; firmware clears on coordinator ACK). */
uint8_t coordinator_fallback_get_soft_fault_count(void);

/**
 * Enqueue an explicit occupancy report for the given endpoint with ACK tracking
 * and retry.  Replaces the raw esp_zb_zcl_set_attribute_val auto-report path.
 *
 * @param ep       Zigbee endpoint (1=main, 2-11=zones)
 * @param occupied New occupancy state
 */
void coordinator_fallback_report_occupancy(uint8_t ep, bool occupied);

/**
 * Start the firmware-side occupancy keep-alive timer (fires every 5 minutes).
 * Reports all 11 endpoint occupancy states as a burst so Z2M always has a
 * fresh snapshot even with no motion.  Call once after sensor_bridge_start().
 */
void coordinator_fallback_start_keepalive(void);
