// SPDX-License-Identifier: MIT
#pragma once

/**
 * Zigbee endpoint, cluster, and attribute definitions for LD2450-ZB-H2.
 */

/* ---- Endpoint numbers ---- */
#define ZB_EP_MAIN          1       /* Main device: overall occupancy + config */
#define ZB_EP_ZONE_BASE     2       /* Zones 1-10 are endpoints 2-11 */
#define ZB_EP_ZONE_COUNT    10
#define ZB_EP_ZONE(n)       (ZB_EP_ZONE_BASE + (n))  /* n = 0..9 */

/* ---- Device type ---- */
#define ZB_DEVICE_ID_OCCUPANCY_SENSOR  0x0107

/* ---- Custom cluster IDs (manufacturer-specific range 0xFC00-0xFFFE) ---- */
#define ZB_CLUSTER_LD2450_CONFIG       0xFC00  /* EP 1: target data + sensor config + zone config */
/* ZB_CLUSTER_LD2450_ZONE 0xFC01 removed — zone config consolidated to EP1 cluster 0xFC00 */

/* ---- Attributes on ZB_CLUSTER_LD2450_CONFIG (EP 1) ---- */
#define ZB_ATTR_TARGET_COUNT           0x0000  /* U8, read-only + reportable */
#define ZB_ATTR_TARGET_COORDS          0x0001  /* CHAR_STRING, read-only + reportable */
#define ZB_ATTR_MAX_DISTANCE           0x0010  /* U16, read-write (0-6000 mm) */
#define ZB_ATTR_ANGLE_LEFT             0x0011  /* U8, read-write (0-90 deg) */
#define ZB_ATTR_ANGLE_RIGHT            0x0012  /* U8, read-write (0-90 deg) */
#define ZB_ATTR_TRACKING_MODE          0x0020  /* U8, read-write (0=multi, 1=single) */
#define ZB_ATTR_COORD_PUBLISHING       0x0021  /* U8, read-write (0=off, 1=on) */
#define ZB_ATTR_OCCUPANCY_COOLDOWN     0x0022  /* U16, read-write (0-300 seconds) */
#define ZB_ATTR_OCCUPANCY_DELAY        0x0023  /* U16, read-write (0-65535 milliseconds) */

/* Crash diagnostics (read-only, for remote debugging) */
#define ZB_ATTR_BOOT_COUNT             0x0030  /* U32, read-only (monotonic boot counter) */
#define ZB_ATTR_RESET_REASON           0x0031  /* U8, read-only (last reset cause) */
#define ZB_ATTR_LAST_UPTIME_SEC        0x0032  /* U32, read-only (uptime before last reset) */
#define ZB_ATTR_MIN_FREE_HEAP          0x0033  /* U32, read-only (min free heap since boot) */

/* ZB_ATTR_RESTART (0x00F0) and ZB_ATTR_FACTORY_RESET (0x00F1) defined in zigbee_ctrl.h */

/* ---- Zone config attributes on EP1 cluster 0xFC00 ---- */
/* Base: 0x0040, 4 attrs per zone, n = 0..9 (firmware 0-indexed; Z2M uses 1-indexed zone_1..zone_10) */
/* Zone N attr range: 0x0040 + N*4 .. 0x0043 + N*4. Zone 9 last attr = 0x006B */
#define ZB_ZONE_ATTR_BASE(n)           (0x0040 + (n) * 4)
#define ZB_ATTR_ZONE_VERTEX_COUNT(n)   (ZB_ZONE_ATTR_BASE(n) + 0)  /* U8,  read-write */
#define ZB_ATTR_ZONE_COORDS(n)         (ZB_ZONE_ATTR_BASE(n) + 1)  /* CHAR_STRING, read-write */
#define ZB_ATTR_ZONE_COOLDOWN(n)       (ZB_ZONE_ATTR_BASE(n) + 2)  /* U16, read-write (0-300 sec) */
#define ZB_ATTR_ZONE_DELAY(n)          (ZB_ZONE_ATTR_BASE(n) + 3)  /* U16, read-write (0-65535 ms) */

/* Max bytes for a zone coords ZCL CHAR_STRING (length prefix byte + CSV payload) */
/* 10 vertices × 2 coords × max 6 chars + 9 commas per pair group + separating commas ≈ 153 chars */
#define ZB_ZONE_COORDS_MAX_LEN         160

/* ---- Fallback mode attributes on EP1 cluster 0xFC00 ---- */
#define ZB_ATTR_FALLBACK_MODE              0x0024  /* U8,  RW+Report  0=normal, 1=HARD fallback active */
#define ZB_ATTR_FALLBACK_COOLDOWN          0x0025  /* U16, RW         main EP fallback cooldown (seconds, default: 300) */
#define ZB_ATTR_HEARTBEAT_ENABLE           0x0026  /* U8,  RW         0=off, 1=expect periodic heartbeat writes */
#define ZB_ATTR_HEARTBEAT_INTERVAL         0x0027  /* U16, RW         expected beat interval in seconds (default: 120) */
#define ZB_ATTR_HEARTBEAT                  0x0028  /* U8,  W          write any value to reset the software watchdog */
#define ZB_ATTR_FALLBACK_ENABLE            0x0029  /* U8,  RW         global enable for soft/hard fallback (default: 0=off) */
#define ZB_ATTR_SOFT_FAULT                 0x002A  /* U8,  R+Report   soft fault counter (firmware-only write; HA observes) */
#define ZB_ATTR_HARD_TIMEOUT_SEC           0x002B  /* U8,  RW         seconds from first soft fault → hard fallback (default: 10) */
#define ZB_ATTR_ACK_TIMEOUT_MS             0x002C  /* U16, RW         APS ACK timeout in ms (default: 2000) */
#define ZB_ATTR_FALLBACK_ZONE_COOL_BASE    0x0070  /* U16, RW         zone N cooldown: base + zone_index (0-9) → 0x0070-0x0079 */

/* ---- Identity strings ---- */
#define ZB_MANUFACTURER_NAME           "\x07""LD2450Z"   /* ZCL string: len byte + chars */
/* DEV identifier: distinguishes dev/coordinator-fallback branch from production in Z2M.
 * IMPORTANT: revert to "\x09""LD2450-H2" before merging to master. */
#define ZB_MODEL_IDENTIFIER            "\x0d""LD2450-H2-DEV"

/* ---- Shared zigbee_ctrl attributes ---- */
#include "zigbee_ctrl.h"

/* ---- Firmware version (derived from version.h) ---- */
#include "version.h"
#define ZB_FW_VERSION_STR              FIRMWARE_VERSION_STRING_PLAIN  /* Plain string for logging */
#define ZB_SW_BUILD_ID                 FIRMWARE_SW_BUILD_ID           /* ZCL string for Zigbee */
