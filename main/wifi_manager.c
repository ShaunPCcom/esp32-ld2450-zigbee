// SPDX-License-Identifier: MIT
#include "wifi_manager.h"

#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "wifi_mgr";

/* NVS namespace and keys */
#define WIFI_NVS_NAMESPACE  "wifi_cfg"
#define WIFI_NVS_KEY_SSID   "ssid"
#define WIFI_NVS_KEY_PASS   "pass"
#define WIFI_NVS_KEY_HOST   "hostname"

/* AP defaults */
#define WIFI_AP_MAX_CONN    4
#define WIFI_STA_MAX_RETRY  5

/* Captive DNS */
#define DNS_PORT        53
#define DNS_BUF_SIZE    512

static volatile wifi_mgr_state_t s_state = WIFI_MGR_STATE_INIT;
static int s_retry_count = 0;
static esp_netif_t *s_netif_ap  = NULL;
static esp_netif_t *s_netif_sta = NULL;

/* ================================================================== */
/*  NVS credential helpers                                             */
/* ================================================================== */

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }

    size_t sl = ssid_len;
    size_t pl = pass_len;
    bool ok = (nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &sl) == ESP_OK &&
               nvs_get_str(h, WIFI_NVS_KEY_PASS, pass, &pl) == ESP_OK &&
               sl > 1);  /* at least 1 char + null */

    nvs_close(h);
    return ok;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, WIFI_NVS_KEY_PASS, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Credentials saved (SSID: %s)", ssid);
    }
    return err;
}

esp_err_t wifi_manager_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, WIFI_NVS_KEY_SSID);
    nvs_erase_key(h, WIFI_NVS_KEY_PASS);
    nvs_erase_key(h, WIFI_NVS_KEY_HOST);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool wifi_manager_has_credentials(void)
{
    char ssid[33] = {};
    char pass[65] = {};
    return load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
}

esp_err_t wifi_manager_save_hostname(const char *hostname)
{
    if (!hostname) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, WIFI_NVS_KEY_HOST, hostname);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool wifi_manager_get_hostname(char *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }
    size_t sl = len;
    bool ok = (nvs_get_str(h, WIFI_NVS_KEY_HOST, buf, &sl) == ESP_OK && sl > 1);
    nvs_close(h);
    return ok;
}

/* ================================================================== */
/*  Captive DNS task                                                   */
/* ================================================================== */

/* Build a minimal DNS A-record response redirecting to captive_ip. */
static int build_dns_response(const uint8_t *query, int query_len,
                              uint8_t *resp, int resp_size,
                              uint32_t captive_ip_be)
{
    if (query_len < 12 || resp_size < query_len + 16) {
        return -1;
    }

    memcpy(resp, query, query_len);

    resp[2] = 0x81;
    resp[3] = 0x80;
    resp[6] = 0x00;
    resp[7] = 0x01;
    resp[8] = 0x00;
    resp[9] = 0x00;
    resp[10] = 0x00;
    resp[11] = 0x00;

    uint8_t *ans = resp + query_len;
    ans[0] = 0xC0;
    ans[1] = 0x0C;
    ans[2] = 0x00;
    ans[3] = 0x01;
    ans[4] = 0x00;
    ans[5] = 0x01;
    ans[6] = 0x00;
    ans[7] = 0x00;
    ans[8] = 0x00;
    ans[9] = 0x01;
    ans[10] = 0x00;
    ans[11] = 0x04;
    memcpy(&ans[12], &captive_ip_be, 4);

    return query_len + 16;
}

static void captive_dns_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(500));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(DNS_PORT),
        .sin_addr   = { .s_addr = INADDR_ANY },
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS listening on port 53");

    uint32_t captive_ip = inet_addr("192.168.4.1");

    static uint8_t buf[DNS_BUF_SIZE];
    static uint8_t resp[DNS_BUF_SIZE + 16];

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (s_state == WIFI_MGR_STATE_AP) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len < 0) {
            break;
        }

        int resp_len = build_dns_response(buf, len, resp, sizeof(resp), captive_ip);
        if (resp_len > 0) {
            sendto(sock, resp, resp_len, 0,
                   (struct sockaddr *)&client_addr, client_len);
        }
    }

    close(sock);
    ESP_LOGI(TAG, "Captive DNS task done");
    vTaskDelete(NULL);
}

/* ================================================================== */
/*  WiFi event handler                                                 */
/* ================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            /* Only connect if we intentionally started STA mode — not when
             * APSTA is used for provisioning mode scans. */
            if (s_state == WIFI_MGR_STATE_STA_CONNECTING) {
                ESP_LOGI(TAG, "STA start — connecting...");
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            if (s_state != WIFI_MGR_STATE_STA_CONNECTING &&
                s_state != WIFI_MGR_STATE_STA_CONNECTED) {
                break; /* not in STA mode, ignore */
            }
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "STA disconnected (reason %u), retry %d/%d",
                     d->reason, s_retry_count + 1, WIFI_STA_MAX_RETRY);
            if (s_retry_count < WIFI_STA_MAX_RETRY) {
                s_retry_count++;
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "STA failed after %d retries — falling back to AP mode", WIFI_STA_MAX_RETRY);
                s_state = WIFI_MGR_STATE_STA_FAILED;
                esp_wifi_stop();
                wifi_manager_start();
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *c = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: station " MACSTR " join, AID=%d",
                     MAC2STR(c->mac), c->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *c = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: station " MACSTR " leave, AID=%d, reason=%d",
                     MAC2STR(c->mac), c->aid, c->reason);
            break;
        }

        default:
            break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        s_state = WIFI_MGR_STATE_STA_CONNECTED;

        /* Start mDNS so the device is reachable at <hostname>.local */
        char hostname[33] = {};
        if (!wifi_manager_get_hostname(hostname, sizeof(hostname))) {
            /* Fallback: derive from STA MAC */
            uint8_t mac[6];
            esp_wifi_get_mac(WIFI_IF_STA, mac);
            snprintf(hostname, sizeof(hostname), "LD2450-%02X%02X%02X",
                     mac[3], mac[4], mac[5]);
        }
        mdns_init();
        mdns_hostname_set(hostname);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS started: %s.local", hostname);
    }
}

/* ================================================================== */
/*  AP mode — modelled on the official softAP example                 */
/* ================================================================== */

static void start_ap_mode(void)
{
    /* Build SSID from last 3 bytes of MAC */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    char ssid[32];
    snprintf(ssid, sizeof(ssid), "LD2450-%02X%02X%02X", mac[3], mac[4], mac[5]);

    /* Password = last 4 MAC bytes as 8 hex chars (min WPA2 length) */
    char password[16];
    snprintf(password, sizeof(password), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = (uint8_t)strlen(ssid),
            .channel        = 1,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };

    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);

    /* Use APSTA so the STA interface can perform scans while AP is active */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = WIFI_MGR_STATE_AP;
    ESP_LOGI(TAG, "AP mode: SSID=\"%s\" password=\"%s\", IP=192.168.4.1", ssid, password);

    xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 4, NULL);
}

/* ================================================================== */
/*  STA mode                                                           */
/* ================================================================== */

static void start_sta_mode(const char *ssid, const char *pass)
{
    /* Set DHCP hostname before connecting */
    char hostname[33] = {};
    if (!wifi_manager_get_hostname(hostname, sizeof(hostname))) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(hostname, sizeof(hostname), "LD2450-%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
    }
    esp_netif_set_hostname(s_netif_sta, hostname);

    wifi_config_t sta_cfg = {};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;

    s_state = WIFI_MGR_STATE_STA_CONNECTING;  /* set before start so event handler sees it */
    ESP_LOGI(TAG, "STA mode: connecting to \"%s\" as \"%s\"", ssid, hostname);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void wifi_manager_init(void)
{
    s_netif_ap  = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    /* Enable WiFi/802.15.4 coexistence — required for Zigbee+WiFi to share the
     * radio. Without this call the coex scheduler is not activated even when
     * CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y. See esp_zigbee_gateway example. */
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_wifi_i154_enable();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "WiFi subsystem initialized");
}

void wifi_manager_start(void)
{
    char ssid[33] = {};
    char pass[65] = {};

    if (s_state == WIFI_MGR_STATE_STA_FAILED) {
        start_ap_mode();
        return;
    }

    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Found stored credentials for \"%s\"", ssid);
        start_sta_mode(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials stored");
        start_ap_mode();
    }
}

wifi_mgr_state_t wifi_manager_get_state(void)
{
    return s_state;
}

bool wifi_manager_is_connected(void)
{
    return s_state == WIFI_MGR_STATE_STA_CONNECTED;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_state == WIFI_MGR_STATE_AP;
}
