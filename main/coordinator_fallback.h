// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Coordinator offline fallback module.
 *
 * When the Zigbee coordinator (Z2M/HA) stops ACKing occupancy reports within
 * ACK_TIMEOUT_MS, the device enters fallback mode and sends On/Off commands
 * directly to bound lights via the Zigbee binding table.
 *
 * Fallback mode is sticky — it persists in NVS across reboots and is only
 * cleared when HA explicitly writes fallback_mode=0.  No firmware code may
 * clear fallback mode except coordinator_fallback_clear().
 *
 * Design notes:
 * - "Always armed": if On/Off bindings exist, fallback can activate. To
 *   prevent fallback, remove the On/Off binding — not a firmware config flag.
 * - "Auto re-arm": after HA clears fallback, the device immediately watches
 *   for ACK failures again.
 * - "Lights stay on": exiting fallback does NOT send Off commands. HA
 *   reconciles light state.
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
 * Returns true if the given endpoint index entered occupancy under fallback.
 * Used by sensor_bridge to switch cooldown values for in-progress sessions.
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
