// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ld2450_zone.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Serialize a zone's vertices to a comma-separated string.
 * Format: "x0,y0,x1,y1,...,x(n-1),y(n-1)"
 * Writes empty string if vertex_count < 3 (disabled zone).
 *
 * @param zone   Source zone (vertex_count determines how many pairs are written)
 * @param buf    Output buffer
 * @param buflen Size of output buffer in bytes
 */
void zone_to_csv(const ld2450_zone_t *zone, char *buf, size_t buflen);

/**
 * Deserialize a comma-separated coordinate string into zone vertices.
 * Reads exactly zone->vertex_count pairs. Returns false if vertex_count
 * is invalid (< 3 or > MAX_ZONE_VERTICES) or the string has insufficient values.
 *
 * The caller must set zone->vertex_count before calling.
 *
 * @param csv    Input CSV string (not modified)
 * @param zone   Zone to populate (vertex_count must already be set)
 * @return true on success, false on bad vertex_count or truncated input
 */
bool csv_to_zone(const char *csv, ld2450_zone_t *zone);

/**
 * Count coordinate pairs in a CSV string without modifying any state.
 * Used for validation: csv_count_pairs(csv) must equal zone->vertex_count
 * before committing a write.
 *
 * @param csv  Comma-separated string, or NULL / empty
 * @return     Number of (x,y) pairs, or 0 for NULL/empty input
 */
int csv_count_pairs(const char *csv);

#ifdef __cplusplus
}
#endif
