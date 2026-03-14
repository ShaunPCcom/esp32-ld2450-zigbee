// SPDX-License-Identifier: MIT
#include "ld2450_zone.h"

static bool point_on_segment(ld2450_point_t p, ld2450_point_t a, ld2450_point_t b)
{
    // Check colinearity via cross product, then bounding box.
    int32_t cross = (int32_t)(p.y_mm - a.y_mm) * (b.x_mm - a.x_mm) -
                    (int32_t)(p.x_mm - a.x_mm) * (b.y_mm - a.y_mm);
    if (cross != 0) return false;

    int16_t minx = (a.x_mm < b.x_mm) ? a.x_mm : b.x_mm;
    int16_t maxx = (a.x_mm > b.x_mm) ? a.x_mm : b.x_mm;
    int16_t miny = (a.y_mm < b.y_mm) ? a.y_mm : b.y_mm;
    int16_t maxy = (a.y_mm > b.y_mm) ? a.y_mm : b.y_mm;

    return (p.x_mm >= minx && p.x_mm <= maxx &&
            p.y_mm >= miny && p.y_mm <= maxy);
}

bool ld2450_zone_contains_point(const ld2450_zone_t *z, ld2450_point_t p)
{
    if (!z || z->vertex_count < 3) return false;

    int n = (int)z->vertex_count;

    // Ray casting to the right — generalized for n vertices
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        ld2450_point_t a = z->v[j];
        ld2450_point_t b = z->v[i];

        // Treat boundary as inside
        if (point_on_segment(p, a, b)) return true;

        // Does edge straddle the horizontal ray at p.y?
        bool cond = ((a.y_mm > p.y_mm) != (b.y_mm > p.y_mm));
        if (cond) {
            // Compute x intersection without float:
            // x_int = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y)
            int32_t dy = (int32_t)(b.y_mm - a.y_mm);
            int32_t num = (int32_t)(p.y_mm - a.y_mm) * (b.x_mm - a.x_mm);
            int32_t x_int = (int32_t)a.x_mm + (num / dy);

            if (x_int >= p.x_mm) {
                inside = !inside;
            }
        }
    }
    return inside;
}

