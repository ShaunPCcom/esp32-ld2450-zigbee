// SPDX-License-Identifier: MIT
#include "ota_check.h"
#include "version.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

#define TAG               "ota_check"
#define OTA_INDEX_URL     "https://shaunpccom.github.io/zigbee-ota-index/ota_index.json"
#define OTA_MFR_CODE      0x131B
#define OTA_IMAGE_TYPE_C6 0x0003
#define NVS_NAMESPACE     "ld2450_cfg"
#define NVS_KEY_INTERVAL  "ota_chk_int"
#define DEFAULT_INTERVAL_H 12
#define INDEX_BUF_SIZE    6144

static bool               s_available = false;
static char               s_latest_version[16] = "";
static esp_timer_handle_t s_timer = NULL;
static TaskHandle_t       s_task  = NULL;
static SemaphoreHandle_t  s_mutex = NULL;
static char               s_buf[INDEX_BUF_SIZE];   /* static — not on task stack */

/* ── Core HTTP check ───────────────────────────────────────────────────── */

static void do_check(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_http_client_config_t cfg = {
        .url                = OTA_INDEX_URL,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .timeout_ms         = 10000,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_mutex);
        return;
    }

    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_mutex);
        return;
    }

    int total = 0, rd;
    while (total < INDEX_BUF_SIZE - 1) {
        rd = esp_http_client_read(client, s_buf + total, INDEX_BUF_SIZE - 1 - total);
        if (rd <= 0) break;
        total += rd;
    }
    s_buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    cJSON *root = cJSON_Parse(s_buf);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        xSemaphoreGive(s_mutex);
        return;
    }

    bool found = false;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        cJSON *mfr = cJSON_GetObjectItem(item, "manufacturerCode");
        cJSON *img = cJSON_GetObjectItem(item, "imageType");
        cJSON *ver = cJSON_GetObjectItem(item, "fileVersion");
        if (!mfr || !img || !ver) continue;
        if ((uint16_t)mfr->valueint != OTA_MFR_CODE)      continue;
        if ((uint16_t)img->valueint != OTA_IMAGE_TYPE_C6)  continue;

        uint32_t latest_hex;
        if (cJSON_IsNumber(ver)) {
            latest_hex = (uint32_t)ver->valuedouble;
        } else if (cJSON_IsString(ver)) {
            latest_hex = (uint32_t)strtoul(ver->valuestring, NULL, 0);
        } else {
            continue;
        }

        if (latest_hex > FIRMWARE_VERSION) {
            s_available = true;
            uint8_t maj = (latest_hex >> 16) & 0xFF;
            uint8_t min = (latest_hex >>  8) & 0xFF;
            uint8_t pat =  latest_hex        & 0xFF;
            snprintf(s_latest_version, sizeof(s_latest_version), "%u.%u.%u", maj, min, pat);
        } else {
            s_available = false;
            s_latest_version[0] = '\0';
        }
        found = true;
        break;
    }
    cJSON_Delete(root);

    if (!found) ESP_LOGW(TAG, "no matching entry in OTA index");
    ESP_LOGI(TAG, "check done: available=%d latest=%s", s_available, s_latest_version);

    xSemaphoreGive(s_mutex);
}

/* ── Background task (triggered by periodic timer) ───────────────────── */

static void check_task_fn(void *arg)
{
    /* Wait 15 s after init so Wi-Fi/routing fully settles before first check */
    vTaskDelay(pdMS_TO_TICKS(15000));
    do_check();

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        do_check();
    }
}

static void timer_cb(void *arg)
{
    if (s_task) xTaskNotifyGive(s_task);
}

/* ── NVS helpers ─────────────────────────────────────────────────────── */

static uint16_t load_interval(void)
{
    nvs_handle_t h;
    uint16_t val = DEFAULT_INTERVAL_H;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, NVS_KEY_INTERVAL, &val);
        nvs_close(h);
    }
    return val;
}

static void start_periodic_timer(uint16_t hours)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }
    const esp_timer_create_args_t args = { .callback = timer_cb, .name = "ota_chk" };
    esp_timer_create(&args, &s_timer);
    uint64_t period_us = (uint64_t)hours * 3600ULL * 1000000ULL;
    esp_timer_start_periodic(s_timer, period_us);
    ESP_LOGI(TAG, "check interval: %u h", hours);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ota_check_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    xTaskCreate(check_task_fn, "ota_check", 6144, NULL, 2, &s_task);
    start_periodic_timer(load_interval());
}

void ota_check_trigger(void)
{
    do_check();  /* blocking — called from HTTP handler task */
}

bool ota_check_available(void)
{
    return s_available;
}

const char *ota_check_latest_version(void)
{
    return s_latest_version;
}

uint16_t ota_check_get_interval_hours(void)
{
    return load_interval();
}

void ota_check_set_interval_hours(uint16_t hours)
{
    if (hours < 1)   hours = 1;
    if (hours > 168) hours = 168;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, NVS_KEY_INTERVAL, hours);
        nvs_commit(h);
        nvs_close(h);
    }
    start_periodic_timer(hours);
}
