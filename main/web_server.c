// SPDX-License-Identifier: MIT
/**
 * LD2450-specific web server — thin wrapper over web_server_base.
 *
 * Handles only LD2450 device endpoints:
 *   GET  /api/config   — full sensor config JSON
 *   POST /api/config   — partial config update
 *   WS   /ws/targets   — 2 Hz target + occupancy stream
 *
 * All WiFi, OTA, system, and diagnostics endpoints are handled by
 * web_server_base and registered automatically in web_server_base_start().
 */
#include "web_server.h"
#include "web_server_base.h"

#include "config_api.h"
#include "sensor_bridge.h"
#include "version.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ld2450.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server";

#define MAX_BODY_LEN 4096

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");
extern const char style_css_start[]  asm("_binary_style_css_start");
extern const char style_css_end[]    asm("_binary_style_css_end");

/* ================================================================== */
/*  Helpers (local to this file)                                      */
/* ================================================================== */

static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY_LEN) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - (size_t)received);
        if (r <= 0) { free(buf); return NULL; }
        received += r;
    }
    buf[received] = '\0';
    return buf;
}

static void send_json(httpd_req_t *req, int http_status, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (http_status == 400) httpd_resp_set_status(req, "400 Bad Request");
    else if (http_status == 500) httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, str ? str : "{}");
    free(str);
}

/* ================================================================== */
/*  GET /api/config                                                    */
/* ================================================================== */

static esp_err_t handle_get_config(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (config_api_get_all(&json) != ESP_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "Failed to read config");
        send_json(req, 500, e); cJSON_Delete(e); return ESP_OK;
    }
    send_json(req, 200, json);
    cJSON_Delete(json);
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/config                                                   */
/* ================================================================== */

static esp_err_t handle_post_config(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "No body or too large (max 4096)");
        send_json(req, 400, e); cJSON_Delete(e); return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "Invalid JSON");
        send_json(req, 400, e); cJSON_Delete(e); return ESP_OK;
    }

    cJSON *item;
#define APPLY_NUM(key, fn, type) \
    if ((item = cJSON_GetObjectItem(root, key)) && cJSON_IsNumber(item)) \
        fn((type)item->valueint)

    APPLY_NUM("max_distance_mm",        config_api_set_max_distance,       uint16_t);
    APPLY_NUM("angle_left_deg",         config_api_set_angle_left,         uint8_t);
    APPLY_NUM("angle_right_deg",        config_api_set_angle_right,        uint8_t);
    APPLY_NUM("tracking_mode",          config_api_set_tracking_mode,      uint8_t);
    APPLY_NUM("publish_coords",         config_api_set_publish_coords,     uint8_t);
    APPLY_NUM("fallback_mode",          config_api_set_fallback_mode,      uint8_t);
    APPLY_NUM("fallback_enable",        config_api_set_fallback_enable,    uint8_t);
    APPLY_NUM("hard_timeout_sec",       config_api_set_hard_timeout,       uint8_t);
    APPLY_NUM("ack_timeout_ms",         config_api_set_ack_timeout,        uint16_t);
    APPLY_NUM("heartbeat_enable",       config_api_set_heartbeat_enable,   uint8_t);
    APPLY_NUM("heartbeat_interval_sec", config_api_set_heartbeat_interval, uint16_t);

    if ((item = cJSON_GetObjectItem(root, "occupancy_cooldown_sec")) && cJSON_IsNumber(item))
        config_api_set_occupancy_cooldown(0, (uint16_t)item->valueint);
    if ((item = cJSON_GetObjectItem(root, "occupancy_delay_ms")) && cJSON_IsNumber(item))
        config_api_set_occupancy_delay(0, (uint16_t)item->valueint);
    if ((item = cJSON_GetObjectItem(root, "fallback_cooldown_sec")) && cJSON_IsNumber(item))
        config_api_set_fallback_cooldown(0, (uint16_t)item->valueint);

#undef APPLY_NUM

    cJSON *zones = cJSON_GetObjectItem(root, "zones");
    if (cJSON_IsArray(zones)) {
        int n = cJSON_GetArraySize(zones);
        for (int i = 0; i < n && i < 10; i++) {
            cJSON *z = cJSON_GetArrayItem(zones, i);
            if (!cJSON_IsObject(z)) continue;
            cJSON *vc = cJSON_GetObjectItem(z, "vertex_count");
            if (vc && cJSON_IsNumber(vc))
                config_api_set_zone_vertex_count((uint8_t)i, (uint8_t)vc->valueint);
            cJSON *coords = cJSON_GetObjectItem(z, "coords");
            if (coords && cJSON_IsString(coords))
                config_api_set_zone_coords((uint8_t)i, coords->valuestring);
            cJSON *cool = cJSON_GetObjectItem(z, "cooldown_sec");
            if (cool && cJSON_IsNumber(cool))
                config_api_set_occupancy_cooldown((uint8_t)(i + 1), (uint16_t)cool->valueint);
            cJSON *delay = cJSON_GetObjectItem(z, "delay_ms");
            if (delay && cJSON_IsNumber(delay))
                config_api_set_occupancy_delay((uint8_t)(i + 1), (uint16_t)delay->valueint);
            cJSON *fcool = cJSON_GetObjectItem(z, "fallback_cooldown_sec");
            if (fcool && cJSON_IsNumber(fcool))
                config_api_set_fallback_cooldown((uint8_t)(i + 1), (uint16_t)fcool->valueint);
        }
    }
    cJSON_Delete(root);

    sensor_bridge_mark_config_dirty();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  WS /ws/targets — 2 Hz target stream                               */
/* ================================================================== */

static httpd_handle_t s_server_handle = NULL;

static esp_err_t handle_ws_targets(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS /ws/targets: client connected fd=%d",
                 httpd_req_to_sockfd(req));
        /* Cache the server handle for ws_push_task */
        s_server_handle = req->handle;
    }
    return ESP_OK;
}

static void ws_push_task(void *arg)
{
    (void)arg;
    static const size_t MAX_FDS = 8;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));  /* 2 Hz */

        if (!s_server_handle) continue;

        size_t fds_count = MAX_FDS;
        int fds[8];
        if (httpd_get_client_list(s_server_handle, &fds_count, fds) != ESP_OK) continue;

        bool has_ws = false;
        for (size_t i = 0; i < fds_count; i++) {
            if (httpd_ws_get_fd_info(s_server_handle, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                has_ws = true; break;
            }
        }
        if (!has_ws) continue;

        ld2450_state_t state;
        if (ld2450_get_state(&state) != ESP_OK) continue;

        char json[192];
        int n = 0;
        n += snprintf(json + n, sizeof(json) - n, "{\"t\":[");
        for (int i = 0; i < 3; i++) {
            if (i) n += snprintf(json + n, sizeof(json) - n, ",");
            n += snprintf(json + n, sizeof(json) - n, "{\"x\":%d,\"y\":%d,\"p\":%s}",
                         (int)state.targets[i].x_mm, (int)state.targets[i].y_mm,
                         state.targets[i].present ? "true" : "false");
        }
        n += snprintf(json + n, sizeof(json) - n, "],\"occ\":%s,\"z\":[",
                     state.occupied_global ? "true" : "false");
        for (int i = 0; i < 10; i++) {
            if (i) n += snprintf(json + n, sizeof(json) - n, ",");
            n += snprintf(json + n, sizeof(json) - n, "%s",
                         state.zone_occupied[i] ? "true" : "false");
        }
        n += snprintf(json + n, sizeof(json) - n, "]}");

        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)json,
            .len = (size_t)n, .final = true, .fragmented = false,
        };
        for (size_t i = 0; i < fds_count; i++) {
            if (httpd_ws_get_fd_info(s_server_handle, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                esp_err_t e = httpd_ws_send_frame_async(s_server_handle, fds[i], &frame);
                if (e != ESP_OK) {
                    ESP_LOGD(TAG, "ws_push: send failed fd=%d (%s)", fds[i], esp_err_to_name(e));
                }
            }
        }
    }
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

esp_err_t web_server_start(void)
{
    web_server_base_config_t cfg = {
        .device_name       = "LD2450",
        .firmware_version  = FIRMWARE_VERSION_STRING,
        .nvs_namespace     = "ld2450_cfg",
        .ota_image_type    = 0x0003,   /* LD2450-C6 */
        .index_html_start  = (const uint8_t *)index_html_start,
        .index_html_size   = (size_t)(index_html_end - index_html_start),
        .app_js_start      = (const uint8_t *)app_js_start,
        .app_js_size       = (size_t)(app_js_end - app_js_start),
        .style_css_start   = (const uint8_t *)style_css_start,
        .style_css_size    = (size_t)(style_css_end - style_css_start),
    };

    esp_err_t err = web_server_base_start(&cfg);
    if (err != ESP_OK) return err;

    web_server_base_register("/api/config",   HTTP_GET,  handle_get_config,  false);
    web_server_base_register("/api/config",   HTTP_POST, handle_post_config, false);
    web_server_base_register("/ws/targets",   HTTP_GET,  handle_ws_targets,  true);

    xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 4, NULL);

    return ESP_OK;
}

void web_server_stop(void)
{
    web_server_base_stop();
    s_server_handle = NULL;
}
