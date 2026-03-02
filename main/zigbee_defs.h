// SPDX-License-Identifier: MIT
#pragma once

/**
 * Zigbee endpoint, cluster, and attribute definitions for LD2450-ZB-H2.
 */

/* ---- Endpoint numbers ---- */
#define ZB_EP_MAIN          1       /* Main device: overall occupancy + config */
#define ZB_EP_ZONE_BASE     2       /* Zones 1-5 are endpoints 2-6 */
#define ZB_EP_ZONE_COUNT    5
#define ZB_EP_ZONE(n)       (ZB_EP_ZONE_BASE + (n))  /* n = 0..4 */

/* ---- Device type ---- */
#define ZB_DEVICE_ID_OCCUPANCY_SENSOR  0x0107

/* ---- Custom cluster IDs (manufacturer-specific range 0xFC00-0xFFFE) ---- */
#define ZB_CLUSTER_LD2450_CONFIG       0xFC00  /* EP 1: target data + sensor config */
#define ZB_CLUSTER_LD2450_ZONE         0xFC01  /* EP 2-6: zone vertex config */

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

#define ZB_ATTR_RESTART                0x00F0  /* U8, write-only (write any value) */

/* ---- Attributes on ZB_CLUSTER_LD2450_ZONE (EP 2-6) ---- */
#define ZB_ATTR_ZONE_X1                0x0000  /* S16, read-write (mm) */
#define ZB_ATTR_ZONE_Y1                0x0001  /* S16, read-write (mm) */
#define ZB_ATTR_ZONE_X2                0x0002  /* S16, read-write (mm) */
#define ZB_ATTR_ZONE_Y2                0x0003  /* S16, read-write (mm) */
#define ZB_ATTR_ZONE_X3                0x0004  /* S16, read-write (mm) */
#define ZB_ATTR_ZONE_Y3                0x0005  /* S16, read-write (mm) */
#define ZB_ATTR_ZONE_X4                0x0006  /* S16, read-write (mm) */
#define ZB_ATTR_ZONE_Y4                0x0007  /* S16, read-write (mm) */
#define ZB_ATTR_ZONE_VERTEX_COUNT      8       /* Total vertex attrs per zone EP */

/* ---- Identity strings ---- */
#define ZB_MANUFACTURER_NAME           "\x07""LD2450Z"   /* ZCL string: len byte + chars */
#define ZB_MODEL_IDENTIFIER            "\x09""LD2450-H2"

/* ---- Firmware version (derived from version.h) ---- */
#include "version.h"
#define ZB_FW_VERSION_STR              FIRMWARE_VERSION_STRING_PLAIN  /* Plain string for logging */
#define ZB_SW_BUILD_ID                 FIRMWARE_SW_BUILD_ID           /* ZCL string for Zigbee */
