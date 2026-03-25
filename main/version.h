// SPDX-License-Identifier: MIT
#pragma once

/**
 * Firmware Version Management - Single Source of Truth
 *
 * This file defines the firmware version using a three-part semantic versioning
 * scheme (MAJOR.MINOR.PATCH). All version representations (hex, string, ZCL)
 * are automatically derived from these three numbers.
 *
 * To release a new version:
 * 1. Update only the three numbers below (FW_VERSION_MAJOR/MINOR/PATCH)
 * 2. Commit changes
 * 3. Create and push git tag (e.g., v1.1.0)
 * 4. Automated workflow builds firmware and creates OTA image
 *
 * The workflow validates that these constants match the git tag, preventing
 * version mismatches between code and releases.
 */

/* ============================================================================
 * SINGLE SOURCE OF TRUTH - Update only these three values for new releases
 * ============================================================================ */

/**
 * Major version number (0-255)
 * Increment for breaking changes or major feature additions
 */
#define FW_VERSION_MAJOR 2

/**
 * Minor version number (0-255)
 * Increment for new features that are backward compatible
 */
#define FW_VERSION_MINOR 1

/**
 * Patch version number (0-255)
 * Increment for bug fixes and minor improvements
 */
#define FW_VERSION_PATCH 2

/* ============================================================================
 * DERIVED CONSTANTS - Do not modify, these are generated automatically
 * ============================================================================ */

/* String helper macros for version construction */
#define _FW_STR(x) #x
#define FW_STR(x) _FW_STR(x)

/**
 * Firmware version as 32-bit hex value for OTA comparison
 * Format: 0x00MMNNPP where MM=major, NN=minor, PP=patch
 * Example: v1.1.0 = 0x00010100
 */
#define FIRMWARE_VERSION \
    ((FW_VERSION_MAJOR << 16) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)

/**
 * Firmware version as human-readable string with 'v' prefix
 * Format: "vMAJOR.MINOR.PATCH"
 * Example: "v1.1.0"
 * Used in boot banner and logging
 */
#define FIRMWARE_VERSION_STRING \
    "v" FW_STR(FW_VERSION_MAJOR) "." FW_STR(FW_VERSION_MINOR) "." FW_STR(FW_VERSION_PATCH)

/**
 * Firmware version as plain string without 'v' prefix
 * Format: "MAJOR.MINOR.PATCH"
 * Example: "1.1.0"
 * Used where 'v' prefix is not wanted
 */
#define FIRMWARE_VERSION_STRING_PLAIN \
    FW_STR(FW_VERSION_MAJOR) "." FW_STR(FW_VERSION_MINOR) "." FW_STR(FW_VERSION_PATCH)

/**
 * Firmware version as ZCL CHAR_STRING (Zigbee Cluster Library format)
 * Format: length_byte + "MAJOR.MINOR.PATCH"
 * Example: "\x05""1.1.0" (5 characters: "1.1.0")
 *
 * The length byte is automatically calculated as 5 characters for all versions
 * from v0.0.0 through v9.9.9 (single digit for each component).
 *
 * Note: This will need adjustment if any version component reaches double digits
 * (e.g., v10.0.0 would need "\x06" prefix). Given the project scope, single
 * digits are expected to be sufficient.
 */
#define FIRMWARE_SW_BUILD_ID \
    "\x05" FW_STR(FW_VERSION_MAJOR) "." FW_STR(FW_VERSION_MINOR) "." FW_STR(FW_VERSION_PATCH)
