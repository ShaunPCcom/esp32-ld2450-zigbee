// SPDX-License-Identifier: MIT
#include "ld2450_zone_csv.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void zone_to_csv(const ld2450_zone_t *zone, char *buf, size_t buflen)
{
    buf[0] = '\0';
    if (zone->vertex_count < 3) return;  /* disabled — emit empty string */

    size_t pos = 0;
    for (int i = 0; i < zone->vertex_count; i++) {
        int written = snprintf(buf + pos, buflen - pos,
            i == 0 ? "%d,%d" : ",%d,%d",
            zone->v[i].x_mm, zone->v[i].y_mm);
        if (written < 0 || (size_t)written >= buflen - pos) break;
        pos += (size_t)written;
    }
}

bool csv_to_zone(const char *csv, ld2450_zone_t *zone)
{
    int n = zone->vertex_count;
    if (n < 3 || n > MAX_ZONE_VERTICES) return false;

    /* Work on a local copy — strtok modifies the buffer */
    char buf[160];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = strtok(buf, ",");
    for (int i = 0; i < n; i++) {
        if (!tok) return false;
        zone->v[i].x_mm = (int16_t)atoi(tok);

        tok = strtok(NULL, ",");
        if (!tok) return false;
        zone->v[i].y_mm = (int16_t)atoi(tok);

        tok = strtok(NULL, ",");
    }
    return true;
}

int csv_count_pairs(const char *csv)
{
    if (!csv || csv[0] == '\0') return 0;
    int commas = 0;
    for (const char *p = csv; *p; p++) {
        if (*p == ',') commas++;
    }
    /* N pairs = 2N values = 2N-1 commas → pairs = (commas + 1) / 2 */
    return (commas + 1) / 2;
}
