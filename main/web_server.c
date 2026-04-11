// SPDX-License-Identifier: MIT
#include "web_server.h"

#include "config_api.h"
#include "sensor_bridge.h"
#include "version.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ld2450.h"
#include "zigbee_ota.h"
#include "ota_check.h"
#ifdef CONFIG_IDF_TARGET_ESP32C6
extern esp_err_t ota_upload_transport_flash(httpd_req_t *req);
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#define MAX_BODY_LEN 4096

/* ================================================================== */
/*  Minimal setup page (served until LittleFS web UI is ready)        */
/* ================================================================== */

static const char SETUP_HTML[] =
    "<!DOCTYPE html>"
    "<html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>LD2450 Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px;color:#333}"
    "h1{margin-bottom:4px}p{margin-top:4px;color:#555}"
    "label{display:block;margin-top:14px;font-weight:600;font-size:.9em}"
    "input,select{width:100%;padding:8px;margin-top:4px;box-sizing:border-box;"
    "border:1px solid #ccc;border-radius:4px;font-size:1em;background:#fff}"
    ".row{display:flex;gap:8px;margin-top:4px;align-items:stretch}"
    ".row select{flex:1;margin-top:0;width:auto}"
    ".scan{padding:8px 12px;background:#f5f5f5;color:#333;border:1px solid #ccc;"
    "border-radius:4px;font-size:1em;cursor:pointer;white-space:nowrap}"
    ".scan:active{background:#ddd}"
    "button.sub{margin-top:20px;padding:12px;background:#0070f3;color:#fff;"
    "border:none;border-radius:6px;font-size:1em;cursor:pointer;width:100%}"
    "button.sub:active{background:#0051cc}"
    ".info{background:#f5f7ff;border-radius:6px;padding:12px;margin-top:20px;"
    "font-size:.85em;line-height:1.8}"
    ".msg{padding:10px;border-radius:4px;margin-top:16px;display:none}"
    ".ok{background:#d4edda;color:#155724}.err{background:#f8d7da;color:#721c24}"
    ".hint{color:#888;font-size:.8em;margin-top:3px}"
    "</style></head><body>"
    "<h1>LD2450 Setup</h1>"
    "<p>Connect this sensor to your WiFi network for web-based configuration.</p>"
    "<label>Device Hostname</label>"
    "<input id=\"h\" type=\"text\" placeholder=\"e.g. LD2450-kitchen\" maxlength=\"32\""
    " autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
    "<p class=\"hint\">Accessible at http://&lt;hostname&gt;/ after setup. Letters, numbers, hyphens only.</p>"
    "<label>WiFi Network</label>"
    "<div class=\"row\">"
    "<select id=\"net\" onchange=\"onNet()\"><option value=\"\">Scanning...</option></select>"
    "<button type=\"button\" class=\"scan\" onclick=\"scan()\">&#8635; Scan</button>"
    "</div>"
    "<input id=\"s\" type=\"text\" placeholder=\"Hidden network SSID\" style=\"display:none\""
    " autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
    "<label>Password</label>"
    "<input id=\"p\" type=\"password\" placeholder=\"Leave blank for open networks\">"
    "<button class=\"sub\" onclick=\"save()\">Save &amp; Connect</button>"
    "<div id=\"msg\" class=\"msg\"></div>"
    "<div class=\"info\">"
    "<b>Firmware:</b> " FIRMWARE_VERSION_STRING "<br>"
    "<b>API:</b> <a href=\"/api/config\">/api/config</a> &bull; "
    "<a href=\"/api/status\">/api/status</a>"
    "</div>"
    "<script>"
    "window.onload=function(){scan();};"
    "function scan(){"
    "var sel=document.getElementById('net');"
    "sel.innerHTML='<option value=\"\">Scanning...</option>';"
    "sel.disabled=true;"
    "document.getElementById('s').style.display='none';"
    "fetch('/api/wifi-scan')"
    ".then(function(r){return r.json();})"
    ".then(function(nets){"
    "sel.innerHTML='';"
    "nets.forEach(function(n){"
    "var o=document.createElement('option');"
    "o.value=n.ssid;"
    "o.textContent=n.ssid+' ('+n.rssi+' dBm)';"
    "sel.appendChild(o);"
    "});"
    "var h=document.createElement('option');"
    "h.value='__hidden__';h.textContent='-- Hidden network --';"
    "sel.appendChild(h);"
    "sel.disabled=false;onNet();"
    "}).catch(function(){"
    "sel.innerHTML='';"
    "var h=document.createElement('option');"
    "h.value='__hidden__';h.textContent='-- Enter manually --';"
    "sel.appendChild(h);"
    "sel.disabled=false;onNet();"
    "});}"
    "function onNet(){"
    "var v=document.getElementById('net').value;"
    "document.getElementById('s').style.display=(v==='__hidden__'?'block':'none');}"
    "function save(){"
    "var sel=document.getElementById('net');"
    "var ssid=sel.value==='__hidden__'?"
    "document.getElementById('s').value.trim():sel.value;"
    "var h=document.getElementById('h').value.trim(),"
    "p=document.getElementById('p').value,"
    "m=document.getElementById('msg');"
    "if(!h){alert('Hostname is required');return;}"
    "if(!/^[A-Za-z0-9\\-]+$/.test(h)){alert('Hostname: letters, numbers and hyphens only');return;}"
    "if(!ssid){alert('SSID is required');return;}"
    "fetch('/api/wifi',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:ssid,password:p,hostname:h})})"
    ".then(function(r){return r.json();})"
    ".then(function(d){"
    "var url=d.hostname?'http://'+d.hostname+'/':null;"
    "m.innerHTML=url?"
    "'Credentials saved \u2014 Please connect to your WiFi and go to"
    " <a href=\"'+url+'\">'+url+'</a>'"
    ":(d.message||'Saved');"
    "m.className='msg ok';m.style.display='block';"
    "}).catch(function(){"
    "m.textContent='Request failed - check connection';"
    "m.className='msg err';m.style.display='block';});}"
    "</script>"
    "</body></html>";

/* Operational mode status page — served when WiFi STA credentials are present */
static const char OPERATIONAL_HTML[] =
    "<!DOCTYPE html>"
    "<html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>LD2450</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px;color:#333}"
    "h1{margin-bottom:4px}"
    "table{width:100%;border-collapse:collapse;margin-top:16px}"
    "td{padding:8px 4px;border-bottom:1px solid #eee;font-size:.9em}"
    "td:first-child{font-weight:600;width:45%}"
    "button{margin-top:24px;padding:12px;background:#dc3545;color:#fff;"
    "border:none;border-radius:6px;font-size:1em;cursor:pointer;width:100%}"
    "button:active{background:#b02a37}"
    ".msg{padding:10px;border-radius:4px;margin-top:16px;display:none}"
    ".ok{background:#d4edda;color:#155724}"
    "</style></head><body>"
    "<h1>LD2450 Status</h1>"
    "<table id=\"tbl\"><tr><td colspan=\"2\">Loading...</td></tr></table>"
    "<button onclick=\"wifiReset()\">Reconfigure WiFi</button>"
    "<div id=\"msg\" class=\"msg\"></div>"
    "<script>"
    "fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
    "var rows=[['Firmware',d.firmware],['WiFi',d.wifi],['Uptime',d.uptime_sec+'s'],"
    "['Free heap',d.free_heap+' B']];"
    "document.getElementById('tbl').innerHTML=rows.map(function(r){"
    "return '<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>';}).join('');"
    "}).catch(function(){});"
    "function wifiReset(){"
    "if(!confirm('Clear WiFi credentials and reboot to setup mode?'))return;"
    "fetch('/api/wifi-reset',{method:'POST'})"
    ".then(function(){"
    "var m=document.getElementById('msg');"
    "m.textContent='Rebooting to setup mode...';"
    "m.className='msg ok';m.style.display='block';"
    "}).catch(function(){alert('Request failed');});}"
    "</script>"
    "</body></html>";

/* ================================================================== */
/*  SPIFFS static file serving                                         */
/* ================================================================== */

static bool s_spiffs_ok = false;

static void mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/www",
        .partition_label        = "www",
        .max_files              = 6,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s) — using embedded fallback",
                 esp_err_to_name(err));
    } else {
        s_spiffs_ok = true;
        ESP_LOGI(TAG, "SPIFFS mounted at /www");
    }
}

static esp_err_t serve_file(httpd_req_t *req, const char *path,
                             const char *content_type)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_app_js(httpd_req_t *req)
{
    return serve_file(req, "/www/app.js", "application/javascript");
}

static esp_err_t handle_style_css(httpd_req_t *req)
{
    return serve_file(req, "/www/style.css", "text/css");
}

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY_LEN) {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + received,
                               req->content_len - (size_t)received);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
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
    if (http_status == 400) {
        httpd_resp_set_status(req, "400 Bad Request");
    } else if (http_status == 500) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_sendstr(req, str ? str : "{}");
    free(str);
}

static void send_redirect(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_sendstr(req, "");
}

/* ================================================================== */
/*  WS /ws/targets — 2 Hz target stream                               */
/* ================================================================== */

static esp_err_t handle_ws_targets(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS /ws/targets: client connected fd=%d",
                 httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    /* Push-only endpoint — ESP-IDF handles close/ping frames internally.
     * Nothing to do here; returning ESP_OK lets the httpd layer clean up. */
    return ESP_OK;
}

/* Push sensor state to all connected WebSocket clients at 2 Hz.
 *
 * The json[] buffer lives on this task's stack for the full 500 ms vTaskDelay.
 * httpd_ws_send_frame_async() queues work to the httpd task which completes
 * the actual socket send well within that window, so the pointer stays valid. */
static void ws_push_task(void *arg)
{
    (void)arg;
    /* Max open sockets matches cfg.max_open_sockets (default 7) */
    static const size_t MAX_FDS = 8;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));  /* 2 Hz */

        if (!s_server) continue;

        /* Quick check: any WebSocket clients connected? */
        size_t fds_count = MAX_FDS;
        int fds[8];
        if (httpd_get_client_list(s_server, &fds_count, fds) != ESP_OK) continue;

        bool has_ws = false;
        for (size_t i = 0; i < fds_count; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                has_ws = true;
                break;
            }
        }
        if (!has_ws) continue;

        /* Read sensor state (thread-safe via spinlock in ld2450 driver) */
        ld2450_state_t state;
        if (ld2450_get_state(&state) != ESP_OK) continue;

        /* Build compact JSON frame */
        char json[192];
        int n = 0;
        n += snprintf(json + n, sizeof(json) - n, "{\"t\":[");
        for (int i = 0; i < 3; i++) {
            if (i) n += snprintf(json + n, sizeof(json) - n, ",");
            n += snprintf(json + n, sizeof(json) - n, "{\"x\":%d,\"y\":%d,\"p\":%s}",
                         (int)state.targets[i].x_mm,
                         (int)state.targets[i].y_mm,
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

        /* Send to every connected WebSocket client */
        httpd_ws_frame_t frame = {
            .type       = HTTPD_WS_TYPE_TEXT,
            .payload    = (uint8_t *)json,
            .len        = (size_t)n,
            .final      = true,
            .fragmented = false,
        };
        for (size_t i = 0; i < fds_count; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                esp_err_t e = httpd_ws_send_frame_async(s_server, fds[i], &frame);
                if (e != ESP_OK) {
                    ESP_LOGD(TAG, "ws_push: send failed fd=%d (%s)", fds[i], esp_err_to_name(e));
                }
            }
        }
        /* json[] remains on stack for the next 500ms delay — valid until the
         * httpd task has long finished processing all queued send operations. */
    }
}

/* ================================================================== */
/*  GET /api/wifi-scan                                                 */
/* ================================================================== */

#define WIFI_SCAN_MAX_APS  20

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true); /* blocking ~1-2s */
    if (err != ESP_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", esp_err_to_name(err));
        send_json(req, 500, e);
        cJSON_Delete(e);
        return ESP_OK;
    }

    uint16_t ap_count = WIFI_SCAN_MAX_APS;
    wifi_ap_record_t *ap_list = calloc(WIFI_SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        esp_wifi_clear_ap_list();
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        if (ap_list[i].ssid[0] == '\0') continue; /* hidden — skip */
        /* Deduplicate: keep first (strongest) occurrence of each SSID */
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)ap_list[i].ssid, (char *)ap_list[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_list[i].rssi);
        cJSON_AddBoolToObject(ap, "open", ap_list[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, ap);
    }
    free(ap_list);

    send_json(req, 200, arr);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ================================================================== */
/*  Captive portal probes                                              */
/* ================================================================== */

/* Android sends GET /generate_204 expecting HTTP 204 for a clear network.
   We return 302 instead — Android opens the captive portal browser. */
static esp_err_t handle_generate_204(httpd_req_t *req)
{
    send_redirect(req, "http://192.168.4.1/");
    return ESP_OK;
}

/* iOS sends GET /hotspot-detect.html */
static esp_err_t handle_hotspot_detect(httpd_req_t *req)
{
    send_redirect(req, "http://192.168.4.1/");
    return ESP_OK;
}

/* Windows sends GET /ncsi.txt */
static esp_err_t handle_ncsi(httpd_req_t *req)
{
    send_redirect(req, "http://192.168.4.1/");
    return ESP_OK;
}

/* ================================================================== */
/*  GET /                                                              */
/* ================================================================== */

static esp_err_t handle_root(httpd_req_t *req)
{
    if (wifi_manager_is_ap_mode()) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, SETUP_HTML);
        return ESP_OK;
    }
    /* Operational mode: serve LittleFS index.html; fallback to embedded page */
    if (s_spiffs_ok) {
        return serve_file(req, "/www/index.html", "text/html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, OPERATIONAL_HTML);
    return ESP_OK;
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
        send_json(req, 500, e);
        cJSON_Delete(e);
        return ESP_OK;
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
        send_json(req, 400, e);
        cJSON_Delete(e);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "Invalid JSON");
        send_json(req, 400, e);
        cJSON_Delete(e);
        return ESP_OK;
    }

    cJSON *item;
#define APPLY_NUM(key, fn, type) \
    if ((item = cJSON_GetObjectItem(root, key)) && cJSON_IsNumber(item)) \
        fn((type)item->valueint)

    APPLY_NUM("max_distance_mm",      config_api_set_max_distance,      uint16_t);
    APPLY_NUM("angle_left_deg",       config_api_set_angle_left,        uint8_t);
    APPLY_NUM("angle_right_deg",      config_api_set_angle_right,       uint8_t);
    APPLY_NUM("tracking_mode",        config_api_set_tracking_mode,     uint8_t);
    APPLY_NUM("publish_coords",       config_api_set_publish_coords,    uint8_t);
    APPLY_NUM("fallback_mode",        config_api_set_fallback_mode,     uint8_t);
    APPLY_NUM("fallback_enable",      config_api_set_fallback_enable,   uint8_t);
    APPLY_NUM("hard_timeout_sec",     config_api_set_hard_timeout,      uint8_t);
    APPLY_NUM("ack_timeout_ms",       config_api_set_ack_timeout,       uint16_t);
    APPLY_NUM("heartbeat_enable",     config_api_set_heartbeat_enable,  uint8_t);
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

    /* Notify sensor bridge so it pushes updated attrs to the ZCL table
     * on the next poll cycle — keeps Z2M / HA in sync with web UI changes. */
    sensor_bridge_mark_config_dirty();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  GET /api/status                                                    */
/* ================================================================== */

static esp_err_t handle_get_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON_AddStringToObject(root, "firmware",   FIRMWARE_VERSION_STRING);
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(int64_t)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_heap",  (double)esp_get_free_heap_size());

    static const char *wifi_state_names[] = {
        [WIFI_MGR_STATE_INIT]          = "init",
        [WIFI_MGR_STATE_AP]            = "ap",
        [WIFI_MGR_STATE_STA_CONNECTING] = "connecting",
        [WIFI_MGR_STATE_STA_CONNECTED]  = "connected",
        [WIFI_MGR_STATE_STA_FAILED]     = "failed",
    };
    wifi_mgr_state_t ws = wifi_manager_get_state();
    cJSON_AddStringToObject(root, "wifi",
        (ws < (wifi_mgr_state_t)(sizeof(wifi_state_names) / sizeof(wifi_state_names[0])))
            ? wifi_state_names[ws] : "unknown");

    send_json(req, 200, root);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/wifi                                                     */
/* ================================================================== */

static esp_err_t handle_post_wifi(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "No body or too large");
        send_json(req, 400, e);
        cJSON_Delete(e);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "Invalid JSON");
        send_json(req, 400, e);
        cJSON_Delete(e);
        return ESP_OK;
    }

    cJSON *ssid_j     = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_j     = cJSON_GetObjectItem(root, "password");
    cJSON *hostname_j = cJSON_GetObjectItem(root, "hostname");

    if (!ssid_j || !cJSON_IsString(ssid_j) || ssid_j->valuestring[0] == '\0') {
        cJSON_Delete(root);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "ssid is required");
        send_json(req, 400, e);
        cJSON_Delete(e);
        return ESP_OK;
    }

    if (!hostname_j || !cJSON_IsString(hostname_j) || hostname_j->valuestring[0] == '\0') {
        cJSON_Delete(root);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "hostname is required");
        send_json(req, 400, e);
        cJSON_Delete(e);
        return ESP_OK;
    }

    /* Copy strings before cJSON_Delete frees the underlying buffers */
    char ssid_buf[33]     = {};
    char pass_buf[65]     = {};
    char hostname_buf[33] = {};
    strncpy(ssid_buf,     ssid_j->valuestring,     sizeof(ssid_buf) - 1);
    strncpy(pass_buf,     (pass_j && cJSON_IsString(pass_j)) ? pass_j->valuestring : "",
                          sizeof(pass_buf) - 1);
    strncpy(hostname_buf, hostname_j->valuestring, sizeof(hostname_buf) - 1);
    cJSON_Delete(root);

    esp_err_t err = wifi_manager_set_credentials(ssid_buf, pass_buf);
    if (err == ESP_OK) {
        err = wifi_manager_save_hostname(hostname_buf);
    }

    cJSON *resp = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "hostname", hostname_buf);
        send_json(req, 200, resp);
    } else {
        cJSON_AddStringToObject(resp, "error", "Failed to save credentials");
        send_json(req, 500, resp);
    }
    cJSON_Delete(resp);

    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/wifi-reset                                               */
/* ================================================================== */

static esp_err_t handle_wifi_reset(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "resetting");
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_clear_credentials();
    esp_restart();
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/restart                                                  */
/* ================================================================== */

static esp_err_t handle_restart(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "restarting");
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/ota/upload — receive .ota file, flash directly           */
/* ================================================================== */

static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    #ifdef CONFIG_IDF_TARGET_ESP32C6
    if (req->content_len == 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "no file");
        send_json(req, 400, e);
        cJSON_Delete(e);
        return ESP_OK;
    }
    esp_err_t ret = ota_upload_transport_flash(req);
    cJSON *resp = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        send_json(req, 200, resp);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", "OTA already in progress");
        send_json(req, 409, resp);
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", "flash failed");
        send_json(req, 500, resp);
    }
    cJSON_Delete(resp);
    #else
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", "not supported on this target");
    send_json(req, 501, e);
    cJSON_Delete(e);
    #endif
    return ESP_OK;
}

/*  POST /api/zb-reset                                                 */
/* ================================================================== */

static esp_err_t handle_zb_reset(httpd_req_t *req)
{
    extern void zigbee_factory_reset(void);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "resetting");
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    zigbee_factory_reset();
    return ESP_OK;
}

/*  POST /api/factory-reset                                            */
/* ================================================================== */

static esp_err_t handle_factory_reset(httpd_req_t *req)
{
    extern void zigbee_full_factory_reset(void);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "resetting");
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    zigbee_full_factory_reset();
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/ota — trigger Wi-Fi firmware update                      */
/* ================================================================== */

static esp_err_t handle_post_ota(httpd_req_t *req)
{
    /* Optional JSON body: { "url": "https://..." } to override the configured index.
     * No body = use the configured OTA index URL (same as background checks). */
    char *body = read_body(req);
    const char *url = ota_check_get_index_url();  /* default: configured URL */
    cJSON *root = NULL;

    if (body) {
        root = cJSON_Parse(body);
        if (root) {
            cJSON *url_j = cJSON_GetObjectItemCaseSensitive(root, "url");
            if (cJSON_IsString(url_j) && url_j->valuestring[0] != '\0') {
                url = url_j->valuestring;  /* valid until cJSON_Delete(root) */
            }
        }
    }

    esp_err_t ret = zigbee_ota_start_wifi_update(url);  /* copies url internally */

    cJSON_Delete(root);
    free(body);

    if (ret == ESP_OK) {
        httpd_resp_set_status(req, "202 Accepted");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
    }

    cJSON *resp = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "update_started");
    } else {
        cJSON_AddStringToObject(resp, "error",
            ret == ESP_ERR_INVALID_STATE ? "OTA already in progress"
                                         : "Failed to start OTA");
    }
    send_json(req, 200, resp);  /* status already set above; 200 = no override */
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  GET /api/ota/status — update availability                         */
/* ================================================================== */

static esp_err_t handle_get_ota_status(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "available", ota_check_available());
    cJSON_AddStringToObject(resp, "current", FIRMWARE_VERSION_STRING_PLAIN);
    cJSON_AddStringToObject(resp, "latest", ota_check_latest_version());
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/ota/check — run immediate check, return updated status  */
/* ================================================================== */

static esp_err_t handle_post_ota_check(httpd_req_t *req)
{
    ota_check_trigger();  /* blocking ~2-3 s */

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "available", ota_check_available());
    cJSON_AddStringToObject(resp, "current", FIRMWARE_VERSION_STRING_PLAIN);
    cJSON_AddStringToObject(resp, "latest", ota_check_latest_version());
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  GET /api/ota/interval                                              */
/* ================================================================== */

static esp_err_t handle_get_ota_interval(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "interval_hours", ota_check_get_interval_hours());
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/ota/interval — { "interval_hours": N }                  */
/* ================================================================== */

static esp_err_t handle_post_ota_interval(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_json(req, 400, NULL); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_json(req, 400, NULL); return ESP_OK; }

    cJSON *h = cJSON_GetObjectItem(root, "interval_hours");
    if (!cJSON_IsNumber(h)) {
        cJSON_Delete(root);
        send_json(req, 400, NULL);
        return ESP_OK;
    }

    ota_check_set_interval_hours((uint16_t)h->valueint);
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  GET /api/ota/index-url                                            */
/* ================================================================== */

static esp_err_t handle_get_ota_index_url(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "url", ota_check_get_index_url());
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/ota/index-url — { "url": "..." }  (empty = reset)      */
/* ================================================================== */

static esp_err_t handle_post_ota_index_url(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_json(req, 400, NULL); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_json(req, 400, NULL); return ESP_OK; }

    cJSON *u = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(u)) {
        cJSON_Delete(root);
        send_json(req, 400, NULL);
        return ESP_OK;
    }

    ota_check_set_index_url(u->valuestring);  /* empty string resets to default */
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "url", ota_check_get_index_url());
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  404 handler — redirect unknown URIs to / (catches all OS probes)  */
/* ================================================================== */

static esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    send_redirect(req, "http://192.168.4.1/");
    return ESP_OK;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

esp_err_t web_server_start(void)
{
    mount_spiffs();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.stack_size        = 8192;  /* JSON serialization needs headroom */
    cfg.max_uri_handlers  = 24;    /* default is 8; enough for all registered handlers */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                    .method = HTTP_GET,  .handler = handle_root           },
        { .uri = "/app.js",              .method = HTTP_GET,  .handler = handle_app_js         },
        { .uri = "/style.css",           .method = HTTP_GET,  .handler = handle_style_css      },
        { .uri = "/generate_204",        .method = HTTP_GET,  .handler = handle_generate_204   },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET,  .handler = handle_hotspot_detect },
        { .uri = "/ncsi.txt",            .method = HTTP_GET,  .handler = handle_ncsi           },
        { .uri = "/api/wifi-scan",       .method = HTTP_GET,  .handler = handle_wifi_scan      },
        { .uri = "/api/config",          .method = HTTP_GET,  .handler = handle_get_config     },
        { .uri = "/api/config",          .method = HTTP_POST, .handler = handle_post_config    },
        { .uri = "/api/status",          .method = HTTP_GET,  .handler = handle_get_status     },
        { .uri = "/api/wifi",            .method = HTTP_POST, .handler = handle_post_wifi      },
        { .uri = "/api/wifi-reset",      .method = HTTP_POST, .handler = handle_wifi_reset     },
        { .uri = "/api/restart",         .method = HTTP_POST, .handler = handle_restart        },
        { .uri = "/api/zb-reset",        .method = HTTP_POST, .handler = handle_zb_reset       },
        { .uri = "/api/factory-reset",   .method = HTTP_POST, .handler = handle_factory_reset  },
        { .uri = "/api/ota",             .method = HTTP_POST, .handler = handle_post_ota          },
        { .uri = "/api/ota/upload",      .method = HTTP_POST, .handler = handle_ota_upload        },
        { .uri = "/api/ota/status",      .method = HTTP_GET,  .handler = handle_get_ota_status    },
        { .uri = "/api/ota/check",       .method = HTTP_POST, .handler = handle_post_ota_check    },
        { .uri = "/api/ota/interval",    .method = HTTP_GET,  .handler = handle_get_ota_interval  },
        { .uri = "/api/ota/interval",    .method = HTTP_POST, .handler = handle_post_ota_interval },
        { .uri = "/api/ota/index-url",   .method = HTTP_GET,  .handler = handle_get_ota_index_url },
        { .uri = "/api/ota/index-url",   .method = HTTP_POST, .handler = handle_post_ota_index_url},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    /* WebSocket endpoint — must set is_websocket = true */
    static const httpd_uri_t ws_uri = {
        .uri          = "/ws/targets",
        .method       = HTTP_GET,
        .handler      = handle_ws_targets,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_not_found);

    /* Start 2 Hz push task (operational mode — does nothing if no WS clients) */
    xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 4, NULL);

    /* Start background OTA update check (initial check after 15 s) */
    ota_check_init();

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
