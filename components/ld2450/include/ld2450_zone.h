// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t x_mm;
    int16_t y_mm;
} ld2450_point_t;

#define MAX_ZONE_VERTICES   10

/* Sensor physical limit is 6m; 7m gives a small margin for legitimate
 * boundary zones while still rejecting clearly out-of-range coordinates
 * (e.g. unit confusion where metres are entered instead of millimetres).
 * x_mm: constrained to [-ZONE_COORD_LIMIT_MM, +ZONE_COORD_LIMIT_MM]
 * y_mm: constrained to [0, ZONE_COORD_LIMIT_MM] — sensor only sees forward */
#define ZONE_COORD_LIMIT_MM 7000

typedef struct {
    uint8_t vertex_count;           // 0 = disabled; 3–MAX_ZONE_VERTICES = active
    ld2450_point_t v[MAX_ZONE_VERTICES];
} ld2450_zone_t;

/**
 * Returns true if point p is inside the polygon (or on its boundary).
 * Uses ray casting; supports any convex or concave polygon with 3–MAX_ZONE_VERTICES vertices.
 * Returns false if vertex_count < 3 (disabled or invalid).
 */
bool ld2450_zone_contains_point(const ld2450_zone_t *z, ld2450_point_t p);

#ifdef __cplusplus
}
#endif

