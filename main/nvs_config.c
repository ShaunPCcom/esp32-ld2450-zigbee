// SPDX-License-Identifier: MIT
#include "nvs_config.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

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
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
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

    /* Load zones as blobs */
    char key[12];
    for (int i = 0; i < 5; i++) {
        snprintf(key, sizeof(key), "zone_%d", i);
        size_t len = sizeof(ld2450_zone_t);
        nvs_get_blob(h, key, &s_cfg.zones[i], &len);
    }

    /* Load occupancy cooldown - try new array format first, fall back to old single value */
    size_t cooldown_len = sizeof(s_cfg.occupancy_cooldown_sec);
    esp_err_t cool_err = nvs_get_blob(h, "occ_cool", s_cfg.occupancy_cooldown_sec, &cooldown_len);
    if (cool_err == ESP_ERR_NVS_NOT_FOUND) {
        /* Try loading old single-value format for backward compatibility */
        uint16_t old_cooldown = 0;
        if (nvs_get_u16(h, "occ_cool", &old_cooldown) == ESP_OK) {
            /* Populate all endpoints with the old single value */
            for (int i = 0; i < 6; i++) {
                s_cfg.occupancy_cooldown_sec[i] = old_cooldown;
            }
            ESP_LOGI(TAG, "Migrated old cooldown value %u to all endpoints", old_cooldown);
        }
    }

    /* Load occupancy delay */
    size_t delay_len = sizeof(s_cfg.occupancy_delay_ms);
    nvs_get_blob(h, "occ_delay", s_cfg.occupancy_delay_ms, &delay_len);

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

esp_err_t nvs_config_save_zone(uint8_t zone_index, const ld2450_zone_t *zone)
{
    if (zone_index >= 5 || !zone) return ESP_ERR_INVALID_ARG;
    s_cfg.zones[zone_index] = *zone;
    char key[12];
    snprintf(key, sizeof(key), "zone_%d", zone_index);
    return nvs_save_blob(key, zone, sizeof(ld2450_zone_t));
}

esp_err_t nvs_config_save_occupancy_cooldown(uint8_t endpoint_index, uint16_t sec)
{
    if (endpoint_index >= 6) return ESP_ERR_INVALID_ARG;
    if (sec > 300) sec = 300;
    s_cfg.occupancy_cooldown_sec[endpoint_index] = sec;
    return nvs_save_blob("occ_cool", s_cfg.occupancy_cooldown_sec, sizeof(s_cfg.occupancy_cooldown_sec));
}

esp_err_t nvs_config_save_occupancy_delay(uint8_t endpoint_index, uint16_t ms)
{
    if (endpoint_index >= 6) return ESP_ERR_INVALID_ARG;
    s_cfg.occupancy_delay_ms[endpoint_index] = ms;
    return nvs_save_blob("occ_delay", s_cfg.occupancy_delay_ms, sizeof(s_cfg.occupancy_delay_ms));
}
