// SPDX-License-Identifier: MIT
#include "web_server.h"

#include "config_api.h"
#include "version.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
    httpd_resp_set_type(req, "text/html");
    /* AP mode = provisioning; anything else = operational (STA connecting or connected) */
    httpd_resp_sendstr(req, wifi_manager_is_ap_mode() ? SETUP_HTML : OPERATIONAL_HTML);
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
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000LL));
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
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.stack_size        = 8192;  /* JSON serialization needs headroom */
    cfg.max_uri_handlers  = 16;    /* default is 8; we register 11+ handlers */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                    .method = HTTP_GET,  .handler = handle_root           },
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
        { .uri = "/api/factory-reset",   .method = HTTP_POST, .handler = handle_factory_reset  },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_not_found);

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
