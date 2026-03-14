// SPDX-License-Identifier: MIT
#include "nvs_config.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

/* ---------------------------------------------------------------------------
 * Migration guard: lock in both old and new zone struct sizes so a padding
 * surprise is caught at compile time rather than silently corrupting data.
 *
 * Old struct (pre-CSV redesign, firmware ≤ v1.1.2):
 *   bool enabled        → 1 byte + 1 byte alignment padding
 *   ld2450_point_t v[4] → 4 × (int16_t x + int16_t y) = 16 bytes
 *   Total               → 18 bytes on ESP32-H2 (RISC-V, little-endian)
 *
 * New struct (post-CSV redesign):
 *   uint8_t vertex_count → 1 byte + 1 byte alignment padding
 *   ld2450_point_t v[10] → 10 × 4 = 40 bytes
 *   Total                → 42 bytes
 * --------------------------------------------------------------------------- */
typedef struct {
    bool           enabled;
    ld2450_point_t v[4];
} ld2450_zone_old_t;

_Static_assert(sizeof(ld2450_zone_old_t) == 18,
    "Old zone struct size mismatch — check padding before migrating");
_Static_assert(sizeof(ld2450_zone_t) == 42,
    "New zone struct size mismatch — update migration detection");

/* ---------------------------------------------------------------------------
 * Migration helpers (file-scoped — migration is internal to this module)
 * --------------------------------------------------------------------------- */
static bool old_zone_has_coords(const ld2450_zone_old_t *z)
{
    for (int i = 0; i < 4; i++) {
        if (z->v[i].x_mm != 0 || z->v[i].y_mm != 0) return true;
    }
    return false;
}

static void migrate_zone_from_old(const ld2450_zone_old_t *old, ld2450_zone_t *out)
{
    memset(out, 0, sizeof(*out));
    if (old_zone_has_coords(old)) {
        out->vertex_count = 4;
        memcpy(out->v, old->v, 4 * sizeof(ld2450_point_t));
        /* v[4..9] remain zero-initialised */
    } else {
        out->vertex_count = 0;  /* was disabled or never configured */
    }
}

static const char *TAG = "nvs_config";
static const char *NVS_NAMESPACE = "ld2450_cfg";

static nvs_config_t s_cfg;
static bool s_initialized = false;

/* Default config values */
static const nvs_config_t DEFAULT_CONFIG = {
    .tracking_mode    = 0,     /* multi */
    .publish_coords   = 0,     /* off */
    .max_distance_mm  = 6000,
    .angle_left_deg   = 60,
    .angle_right_deg  = 60,
    .bt_disabled      = 1,     /* BT off by default */
    .zones = {
        { .vertex_count = 0 }, { .vertex_count = 0 }, { .vertex_count = 0 },
        { .vertex_count = 0 }, { .vertex_count = 0 }, { .vertex_count = 0 },
        { .vertex_count = 0 }, { .vertex_count = 0 }, { .vertex_count = 0 },
        { .vertex_count = 0 },
    },
    .occupancy_cooldown_sec = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},   /* 11 entries: main + 10 zones */
    .occupancy_delay_ms     = {250, 250, 250, 250, 250, 250, 250, 250, 250, 250, 250},
};

static esp_err_t nvs_save_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_u16(const char *key, uint16_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_config_init(void)
{
    /* Start with defaults */
    s_cfg = DEFAULT_CONFIG;

    nvs_handle_t h;
    /* NVS_READWRITE required: zone migration may write back new format blobs on first boot */
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        s_initialized = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s, using defaults", esp_err_to_name(err));
        s_initialized = true;
        return ESP_OK;
    }

    /* Load each field, keeping default if not found */
    nvs_get_u8(h, "track_mode", &s_cfg.tracking_mode);
    nvs_get_u8(h, "pub_coords", &s_cfg.publish_coords);
    nvs_get_u16(h, "max_dist", &s_cfg.max_distance_mm);
    nvs_get_u8(h, "angle_l", &s_cfg.angle_left_deg);
    nvs_get_u8(h, "angle_r", &s_cfg.angle_right_deg);
    nvs_get_u8(h, "bt_off", &s_cfg.bt_disabled);

    /* Load zones: three-way detection — new format, old format (migrate), or missing (default) */
    char key[12];
    for (int i = 0; i < 10; i++) {
        snprintf(key, sizeof(key), "zone_%d", i);
        s_cfg.zones[i] = (ld2450_zone_t){0};  /* zero-init: vertex_count=0 = disabled */

        /* Try new format first */
        size_t len = sizeof(ld2450_zone_t);
        if (nvs_get_blob(h, key, &s_cfg.zones[i], &len) == ESP_OK
                && len == sizeof(ld2450_zone_t)) {
            continue;  /* new format, use as-is */
        }

        /* Zones 5-9 never existed in old firmware — no migration needed */
        if (i >= 5) continue;

        /* Try old format (zones 0-4 only) */
        ld2450_zone_old_t old = {0};
        size_t old_len = sizeof(old);
        if (nvs_get_blob(h, key, &old, &old_len) == ESP_OK
                && old_len == sizeof(ld2450_zone_old_t)) {
            migrate_zone_from_old(&old, &s_cfg.zones[i]);
            ESP_LOGI(TAG, "zone_%d: migrated from v1 format (vertex_count=%d)",
                     i, s_cfg.zones[i].vertex_count);
            /* Write back in new format — overwrites old blob under the same key */
            esp_err_t save_err = nvs_set_blob(h, key, &s_cfg.zones[i], sizeof(ld2450_zone_t));
            if (save_err == ESP_OK) {
                nvs_commit(h);
                ESP_LOGI(TAG, "zone_%d: migration saved", i);
            } else {
                ESP_LOGW(TAG, "zone_%d: migration write failed (%s), will retry next boot",
                         i, esp_err_to_name(save_err));
            }
            continue;
        }
        /* Key missing or unrecognised size — leave disabled (already zero-init above) */
    }

    /* Load occupancy cooldown — handle: [11] blob, old [6] blob, old single u16 */
    {
        size_t cooldown_len = sizeof(s_cfg.occupancy_cooldown_sec);
        if (nvs_get_blob(h, "occ_cool", s_cfg.occupancy_cooldown_sec, &cooldown_len) != ESP_OK
                || cooldown_len != sizeof(s_cfg.occupancy_cooldown_sec)) {
            /* Try old [6] blob */
            uint16_t old_cool[6] = {0};
            size_t old_len = sizeof(old_cool);
            if (nvs_get_blob(h, "occ_cool", old_cool, &old_len) == ESP_OK
                    && old_len == sizeof(old_cool)) {
                memcpy(s_cfg.occupancy_cooldown_sec, old_cool, sizeof(old_cool));
                /* indices 6-10 remain at default 0 */
                ESP_LOGI(TAG, "cooldown: migrated from [6] to [11]");
            } else {
                /* Try even older single-value u16 */
                uint16_t single = 0;
                if (nvs_get_u16(h, "occ_cool", &single) == ESP_OK) {
                    for (int i = 0; i < 11; i++) s_cfg.occupancy_cooldown_sec[i] = single;
                    ESP_LOGI(TAG, "cooldown: migrated single value %u to all endpoints", single);
                }
            }
        }
    }

    /* Load occupancy delay — handle: [11] blob, old [6] blob */
    {
        size_t delay_len = sizeof(s_cfg.occupancy_delay_ms);
        if (nvs_get_blob(h, "occ_delay", s_cfg.occupancy_delay_ms, &delay_len) != ESP_OK
                || delay_len != sizeof(s_cfg.occupancy_delay_ms)) {
            /* Try old [6] blob */
            uint16_t old_delay[6] = {0};
            size_t old_len = sizeof(old_delay);
            if (nvs_get_blob(h, "occ_delay", old_delay, &old_len) == ESP_OK
                    && old_len == sizeof(old_delay)) {
                memcpy(s_cfg.occupancy_delay_ms, old_delay, sizeof(old_delay));
                /* indices 6-10 keep default 250ms already in s_cfg */
                ESP_LOGI(TAG, "delay: migrated from [6] to [11]");
            }
        }
    }

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: dist=%u left=%u right=%u bt_off=%u mode=%u coords=%u",
             s_cfg.max_distance_mm, s_cfg.angle_left_deg, s_cfg.angle_right_deg,
             s_cfg.bt_disabled, s_cfg.tracking_mode, s_cfg.publish_coords);

    s_initialized = true;
    return ESP_OK;
}

esp_err_t nvs_config_get(nvs_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    *out = s_cfg;
    return ESP_OK;
}

esp_err_t nvs_config_save_tracking_mode(uint8_t mode)
{
    s_cfg.tracking_mode = mode;
    return nvs_save_u8("track_mode", mode);
}

esp_err_t nvs_config_save_publish_coords(uint8_t enabled)
{
    s_cfg.publish_coords = enabled;
    return nvs_save_u8("pub_coords", enabled);
}

esp_err_t nvs_config_save_max_distance(uint16_t mm)
{
    if (mm > 6000) mm = 6000;
    s_cfg.max_distance_mm = mm;
    return nvs_save_u16("max_dist", mm);
}

esp_err_t nvs_config_save_angle_left(uint8_t deg)
{
    if (deg > 90) deg = 90;
    s_cfg.angle_left_deg = deg;
    return nvs_save_u8("angle_l", deg);
}

esp_err_t nvs_config_save_angle_right(uint8_t deg)
{
    if (deg > 90) deg = 90;
    s_cfg.angle_right_deg = deg;
    return nvs_save_u8("angle_r", deg);
}

esp_err_t nvs_config_save_bt_disabled(uint8_t disabled)
{
    s_cfg.bt_disabled = disabled;
    return nvs_save_u8("bt_off", disabled);
}

void nvs_config_update_zone_cache(uint8_t zone_index, const ld2450_zone_t *zone)
{
    if (zone_index >= 10 || !zone) return;
    s_cfg.zones[zone_index] = *zone;
}

esp_err_t nvs_config_save_zone(uint8_t zone_index, const ld2450_zone_t *zone)
{
    if (zone_index >= 10 || !zone) return ESP_ERR_INVALID_ARG;
    s_cfg.zones[zone_index] = *zone;
    char key[12];
    snprintf(key, sizeof(key), "zone_%d", zone_index);
    return nvs_save_blob(key, zone, sizeof(ld2450_zone_t));
}

esp_err_t nvs_config_save_occupancy_cooldown(uint8_t endpoint_index, uint16_t sec)
{
    if (endpoint_index >= 11) return ESP_ERR_INVALID_ARG;
    if (sec > 300) sec = 300;
    s_cfg.occupancy_cooldown_sec[endpoint_index] = sec;
    return nvs_save_blob("occ_cool", s_cfg.occupancy_cooldown_sec, sizeof(s_cfg.occupancy_cooldown_sec));
}

esp_err_t nvs_config_save_occupancy_delay(uint8_t endpoint_index, uint16_t ms)
{
    if (endpoint_index >= 11) return ESP_ERR_INVALID_ARG;
    s_cfg.occupancy_delay_ms[endpoint_index] = ms;
    return nvs_save_blob("occ_delay", s_cfg.occupancy_delay_ms, sizeof(s_cfg.occupancy_delay_ms));
}
