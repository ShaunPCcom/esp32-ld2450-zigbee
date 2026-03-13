// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "crash_diag.h"
#include "ld2450.h"
#include "ld2450_cmd.h"
#include "nvs_config.h"
#include "zigbee_signal_handlers.h"

static const char *TAG = "ld2450_cli";

static int32_t m_to_mm(float m)
{
    float mmf = m * 1000.0f;
    if (mmf >= 0) return (int32_t)(mmf + 0.5f);
    return (int32_t)(mmf - 0.5f);
}

static void print_help(void)
{
    printf(
        "\nLD2450 CLI commands:\n"
        "  ld help\n"
        "  ld state\n"
        "  ld en <0|1>\n"
        "  ld mode <single|multi>\n"
        "  ld zones\n"
        "  ld zone <1-5> <on|off>\n"
        "  ld zone <1-5> on x1 y1 x2 y2 x3 y3 x4 y4   (meters)\n"
        "  ld maxdist <mm>              (0-6000)\n"
        "  ld angle <left> <right>      (0-90 degrees)\n"
        "  ld bt <on|off>\n"
        "  ld coords <on|off>\n"
        "  ld cooldown [seconds]        (set main, show all if no value)\n"
        "  ld cooldown zone <1-5> <sec> (set zone cooldown)\n"
        "  ld cooldown all <seconds>    (set all endpoints)\n"
        "  ld delay [milliseconds]      (set main, show all if no value)\n"
        "  ld delay zone <1-5> <ms>     (set zone delay)\n"
        "  ld delay all <milliseconds>  (set all endpoints)\n"
        "  ld config\n"
        "  ld diag                      (show crash diagnostics)\n"
        "  ld nvs                       (test NVS health)\n"
        "  ld reboot\n"
        "  ld factory-reset             (FULL reset: erase Zigbee + config)\n\n"
    );
}

static void print_state(void)
{
    ld2450_state_t s;
    if (ld2450_get_state(&s) != ESP_OK) {
        printf("state: error\n");
        return;
    }

    printf("state: occupied=%d raw_count=%u eff_count=%u zone_bitmap=0x%02x\n",
           (int)s.occupied_global,
           (unsigned)s.target_count_raw,
           (unsigned)s.target_count_effective,
           (unsigned)s.zone_bitmap);

    for (int i = 0; i < 3; i++) {
        if (s.targets[i].present) {
            printf("  T%d: x=%d y=%d speed=%d\n",
                   i + 1, (int)s.targets[i].x_mm, (int)s.targets[i].y_mm, (int)s.targets[i].speed);
        }
    }
    if (s.target_count_effective > 0) {
        printf("selected: x_mm=%d y_mm=%d speed=%d\n",
               (int)s.selected.x_mm, (int)s.selected.y_mm, (int)s.selected.speed);
    }
}

static void print_zones(void)
{
    ld2450_zone_t z[10];
    if (ld2450_get_zones(z, 10) != ESP_OK) {
        printf("zones: error\n");
        return;
    }

    for (int i = 0; i < 10; i++) {
        printf("zone%d: %s  vertices=%u\n",
               i + 1,
               z[i].vertex_count >= 3 ? "on " : "off",
               z[i].vertex_count);
    }
}

static void print_config(void)
{
    nvs_config_t cfg;
    if (nvs_config_get(&cfg) != ESP_OK) {
        printf("config: error\n");
        return;
    }
    printf("config: max_dist=%u angle_l=%u angle_r=%u bt_off=%u mode=%s coords=%s\n",
           cfg.max_distance_mm, cfg.angle_left_deg, cfg.angle_right_deg,
           cfg.bt_disabled,
           cfg.tracking_mode ? "single" : "multi",
           cfg.publish_coords ? "on" : "off");
    printf("cooldown: main=%u z1=%u z2=%u z3=%u z4=%u z5=%u z6=%u z7=%u z8=%u z9=%u z10=%u sec\n",
           cfg.occupancy_cooldown_sec[0],  cfg.occupancy_cooldown_sec[1],
           cfg.occupancy_cooldown_sec[2],  cfg.occupancy_cooldown_sec[3],
           cfg.occupancy_cooldown_sec[4],  cfg.occupancy_cooldown_sec[5],
           cfg.occupancy_cooldown_sec[6],  cfg.occupancy_cooldown_sec[7],
           cfg.occupancy_cooldown_sec[8],  cfg.occupancy_cooldown_sec[9],
           cfg.occupancy_cooldown_sec[10]);
    printf("delay: main=%u z1=%u z2=%u z3=%u z4=%u z5=%u z6=%u z7=%u z8=%u z9=%u z10=%u ms\n",
           cfg.occupancy_delay_ms[0],  cfg.occupancy_delay_ms[1],
           cfg.occupancy_delay_ms[2],  cfg.occupancy_delay_ms[3],
           cfg.occupancy_delay_ms[4],  cfg.occupancy_delay_ms[5],
           cfg.occupancy_delay_ms[6],  cfg.occupancy_delay_ms[7],
           cfg.occupancy_delay_ms[8],  cfg.occupancy_delay_ms[9],
           cfg.occupancy_delay_ms[10]);
}

static void print_diag(void)
{
    crash_diag_data_t diag;
    if (crash_diag_get_data(&diag) != ESP_OK) {
        printf("diag: error\n");
        return;
    }
    printf("Crash Diagnostics:\n");
    printf("  boot_count:      %" PRIu32 "\n", diag.boot_count);
    printf("  reset_reason:    %u (%s)\n", diag.reset_reason,
           crash_diag_reset_reason_str(diag.reset_reason));
    printf("  last_uptime:     %" PRIu32 " sec\n", diag.last_uptime_sec);
    printf("  min_free_heap:   %" PRIu32 " bytes\n", diag.min_free_heap);
}

static void cli_task(void *arg)
{
    (void)arg;

    print_help();

    const uart_port_t console_uart = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

    char line[256];
    size_t len = 0;

    while (1) {
        uint8_t ch;
        int n = uart_read_bytes(console_uart, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        // Echo
        uart_write_bytes(console_uart, (const char *)&ch, 1);

        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            len = 0;

            char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;

            if (strncmp(p, "ld", 2) != 0 || (p[2] && !isspace((unsigned char)p[2]) && p[2] != '\0')) {
                continue;
            }
            p += 2;
            while (*p && isspace((unsigned char)*p)) p++;

            char *cmd = strtok(p, " \t\r\n");
            if (!cmd) { print_help(); continue; }

            if (strcmp(cmd, "help") == 0) { print_help(); continue; }
            if (strcmp(cmd, "state") == 0) { print_state(); continue; }
            if (strcmp(cmd, "config") == 0) { print_config(); continue; }
            if (strcmp(cmd, "diag") == 0) { print_diag(); continue; }

            if (strcmp(cmd, "en") == 0) {
                char *v = strtok(NULL, " \t\r\n");
                if (!v) { printf("usage: ld en <0|1>\n"); continue; }
                int en = atoi(v);
                ld2450_set_enabled(en ? true : false);
                printf("enabled=%d\n", en ? 1 : 0);
                continue;
            }

            if (strcmp(cmd, "mode") == 0) {
                char *v = strtok(NULL, " \t\r\n");
                if (!v) { printf("usage: ld mode <single|multi>\n"); continue; }
                if (strcmp(v, "single") == 0) {
                    ld2450_set_tracking_mode(LD2450_TRACK_SINGLE);
                    nvs_config_save_tracking_mode(1);
                    printf("mode=single (saved)\n");
                } else if (strcmp(v, "multi") == 0) {
                    ld2450_set_tracking_mode(LD2450_TRACK_MULTI);
                    nvs_config_save_tracking_mode(0);
                    printf("mode=multi (saved)\n");
                } else {
                    printf("usage: ld mode <single|multi>\n");
                }
                continue;
            }

            if (strcmp(cmd, "coords") == 0) {
                char *v = strtok(NULL, " \t\r\n");
                if (!v) { printf("usage: ld coords <on|off>\n"); continue; }
                bool on = strcmp(v, "on") == 0;
                ld2450_set_publish_coords(on);
                nvs_config_save_publish_coords(on ? 1 : 0);
                printf("coords=%s (saved)\n", on ? "on" : "off");
                continue;
            }

            if (strcmp(cmd, "cooldown") == 0) {
                char *arg1 = strtok(NULL, " \t\r\n");
                if (!arg1) {
                    /* No arguments - display all values */
                    nvs_config_t cfg;
                    if (nvs_config_get(&cfg) == ESP_OK) {
                        printf("cooldown: main=%u zone1=%u zone2=%u zone3=%u zone4=%u zone5=%u sec\n",
                               cfg.occupancy_cooldown_sec[0], cfg.occupancy_cooldown_sec[1],
                               cfg.occupancy_cooldown_sec[2], cfg.occupancy_cooldown_sec[3],
                               cfg.occupancy_cooldown_sec[4], cfg.occupancy_cooldown_sec[5]);
                    } else {
                        printf("cooldown: error reading config\n");
                    }
                    continue;
                }

                /* Check for "zone N value" or "all value" syntax */
                if (strcmp(arg1, "zone") == 0) {
                    char *zone_str = strtok(NULL, " \t\r\n");
                    char *val_str = strtok(NULL, " \t\r\n");
                    if (!zone_str || !val_str) {
                        printf("usage: ld cooldown zone <1-5> <seconds>\n");
                        continue;
                    }
                    int zone = atoi(zone_str);
                    if (zone < 1 || zone > 10) {
                        printf("zone must be 1-10\n");
                        continue;
                    }
                    uint16_t sec = (uint16_t)atoi(val_str);
                    if (sec > 300) {
                        printf("cooldown must be 0-300 seconds\n");
                        continue;
                    }
                    esp_err_t err = nvs_config_save_occupancy_cooldown((uint8_t)zone, sec);
                    if (err == ESP_OK) {
                        printf("zone%d cooldown=%u sec (saved)\n", zone, sec);
                    } else {
                        printf("zone%d cooldown=%u sec BUT NVS SAVE FAILED: %s\n", zone, sec, esp_err_to_name(err));
                    }
                    continue;
                } else if (strcmp(arg1, "all") == 0) {
                    char *val_str = strtok(NULL, " \t\r\n");
                    if (!val_str) {
                        printf("usage: ld cooldown all <seconds>\n");
                        continue;
                    }
                    uint16_t sec = (uint16_t)atoi(val_str);
                    if (sec > 300) {
                        printf("cooldown must be 0-300 seconds\n");
                        continue;
                    }
                    /* Set all 6 endpoints */
                    bool all_ok = true;
                    for (uint8_t i = 0; i < 6; i++) {
                        esp_err_t err = nvs_config_save_occupancy_cooldown(i, sec);
                        if (err != ESP_OK) {
                            printf("endpoint %u save FAILED: %s\n", i, esp_err_to_name(err));
                            all_ok = false;
                        }
                    }
                    if (all_ok) {
                        printf("all endpoints cooldown=%u sec (saved)\n", sec);
                    }
                    continue;
                } else {
                    /* Single argument - set main endpoint */
                    uint16_t sec = (uint16_t)atoi(arg1);
                    if (sec > 300) {
                        printf("cooldown must be 0-300 seconds\n");
                        continue;
                    }
                    esp_err_t err = nvs_config_save_occupancy_cooldown(0, sec);
                    if (err == ESP_OK) {
                        printf("main cooldown=%u sec (saved)\n", sec);
                    } else {
                        printf("main cooldown=%u sec BUT NVS SAVE FAILED: %s\n", sec, esp_err_to_name(err));
                    }
                    continue;
                }
            }

            if (strcmp(cmd, "delay") == 0) {
                char *arg1 = strtok(NULL, " \t\r\n");
                if (!arg1) {
                    /* No arguments - display all values */
                    nvs_config_t cfg;
                    if (nvs_config_get(&cfg) == ESP_OK) {
                        printf("delay: main=%u zone1=%u zone2=%u zone3=%u zone4=%u zone5=%u ms\n",
                               cfg.occupancy_delay_ms[0], cfg.occupancy_delay_ms[1],
                               cfg.occupancy_delay_ms[2], cfg.occupancy_delay_ms[3],
                               cfg.occupancy_delay_ms[4], cfg.occupancy_delay_ms[5]);
                    } else {
                        printf("delay: error reading config\n");
                    }
                    continue;
                }

                /* Check for "zone N value" or "all value" syntax */
                if (strcmp(arg1, "zone") == 0) {
                    char *zone_str = strtok(NULL, " \t\r\n");
                    char *val_str = strtok(NULL, " \t\r\n");
                    if (!zone_str || !val_str) {
                        printf("usage: ld delay zone <1-5> <milliseconds>\n");
                        continue;
                    }
                    int zone = atoi(zone_str);
                    if (zone < 1 || zone > 10) {
                        printf("zone must be 1-10\n");
                        continue;
                    }
                    uint16_t ms = (uint16_t)atoi(val_str);
                    esp_err_t err = nvs_config_save_occupancy_delay((uint8_t)zone, ms);
                    if (err == ESP_OK) {
                        printf("zone%d delay=%u ms (saved)\n", zone, ms);
                    } else {
                        printf("zone%d delay=%u ms BUT NVS SAVE FAILED: %s\n", zone, ms, esp_err_to_name(err));
                    }
                    continue;
                } else if (strcmp(arg1, "all") == 0) {
                    char *val_str = strtok(NULL, " \t\r\n");
                    if (!val_str) {
                        printf("usage: ld delay all <milliseconds>\n");
                        continue;
                    }
                    uint16_t ms = (uint16_t)atoi(val_str);
                    /* Set all 6 endpoints */
                    bool all_ok = true;
                    for (uint8_t i = 0; i < 6; i++) {
                        esp_err_t err = nvs_config_save_occupancy_delay(i, ms);
                        if (err != ESP_OK) {
                            printf("endpoint %u save FAILED: %s\n", i, esp_err_to_name(err));
                            all_ok = false;
                        }
                    }
                    if (all_ok) {
                        printf("all endpoints delay=%u ms (saved)\n", ms);
                    }
                    continue;
                } else {
                    /* Single argument - set main endpoint */
                    uint16_t ms = (uint16_t)atoi(arg1);
                    esp_err_t err = nvs_config_save_occupancy_delay(0, ms);
                    if (err == ESP_OK) {
                        printf("main delay=%u ms (saved)\n", ms);
                    } else {
                        printf("main delay=%u ms BUT NVS SAVE FAILED: %s\n", ms, esp_err_to_name(err));
                    }
                    continue;
                }
            }

            if (strcmp(cmd, "maxdist") == 0) {
                char *v = strtok(NULL, " \t\r\n");
                if (!v) { printf("usage: ld maxdist <mm> (0-6000)\n"); continue; }
                uint16_t mm = (uint16_t)atoi(v);
                nvs_config_save_max_distance(mm);

                nvs_config_t cfg;
                nvs_config_get(&cfg);
                ld2450_cmd_apply_distance_angle(cfg.max_distance_mm,
                                                 cfg.angle_left_deg,
                                                 cfg.angle_right_deg);
                printf("maxdist=%u mm (saved, applied)\n", cfg.max_distance_mm);
                continue;
            }

            if (strcmp(cmd, "angle") == 0) {
                char *lv = strtok(NULL, " \t\r\n");
                char *rv = strtok(NULL, " \t\r\n");
                if (!lv || !rv) { printf("usage: ld angle <left> <right> (0-90)\n"); continue; }
                uint8_t left = (uint8_t)atoi(lv);
                uint8_t right = (uint8_t)atoi(rv);
                nvs_config_save_angle_left(left);
                nvs_config_save_angle_right(right);

                nvs_config_t cfg;
                nvs_config_get(&cfg);
                ld2450_cmd_apply_distance_angle(cfg.max_distance_mm,
                                                 cfg.angle_left_deg,
                                                 cfg.angle_right_deg);
                printf("angle left=%u right=%u (saved, applied)\n",
                       cfg.angle_left_deg, cfg.angle_right_deg);
                continue;
            }

            if (strcmp(cmd, "bt") == 0) {
                char *v = strtok(NULL, " \t\r\n");
                if (!v) { printf("usage: ld bt <on|off>\n"); continue; }
                bool on = strcmp(v, "on") == 0;
                ld2450_cmd_set_bluetooth(on);
                nvs_config_save_bt_disabled(on ? 0 : 1);
                printf("bt=%s (saved, restart sensor to take effect)\n", on ? "on" : "off");
                continue;
            }

            if (strcmp(cmd, "zones") == 0) { print_zones(); continue; }

            if (strcmp(cmd, "zone") == 0) {
                char *zid = strtok(NULL, " \t\r\n");
                char *onoff = strtok(NULL, " \t\r\n");
                if (!zid || !onoff) { printf("usage: ld zone <1-5> <on|off> [coords...]\n"); continue; }

                int zi = atoi(zid) - 1;
                if (zi < 0 || zi >= 10) { printf("zone id must be 1-10\n"); continue; }

                ld2450_zone_t all[10];
                if (ld2450_get_zones(all, 10) != ESP_OK) { printf("zones: error\n"); continue; }
                ld2450_zone_t z = all[zi];

                if (strcmp(onoff, "off") == 0) {
                    z.vertex_count = 0;
                    if (ld2450_set_zone((size_t)zi, &z) == ESP_OK) {
                        esp_err_t err = nvs_config_save_zone((uint8_t)zi, &z);
                        if (err == ESP_OK) {
                            printf("zone%d disabled (saved)\n", zi + 1);
                        } else {
                            printf("zone%d disabled BUT NVS SAVE FAILED: %s\n", zi + 1, esp_err_to_name(err));
                        }
                    } else {
                        printf("zone%d update failed\n", zi + 1);
                    }
                    continue;
                }

                if (strcmp(onoff, "on") != 0) {
                    printf("usage: ld zone <1-5> <on|off> [coords...]\n");
                    continue;
                }

                char *coords[8];
                for (int i = 0; i < 8; i++) coords[i] = strtok(NULL, " \t\r\n");

                /* "on" without coords: mark active only if currently disabled */
                if (z.vertex_count < 4) z.vertex_count = 4;

                if (!coords[0]) {
                    if (ld2450_set_zone((size_t)zi, &z) == ESP_OK) {
                        esp_err_t err = nvs_config_save_zone((uint8_t)zi, &z);
                        if (err == ESP_OK) {
                            printf("zone%d enabled (saved)\n", zi + 1);
                        } else {
                            printf("zone%d enabled BUT NVS SAVE FAILED: %s\n", zi + 1, esp_err_to_name(err));
                        }
                    } else {
                        printf("zone%d update failed\n", zi + 1);
                    }
                    continue;
                }

                for (int i = 0; i < 8; i++) {
                    if (!coords[i]) {
                        printf("usage: ld zone <1-5> on x1 y1 x2 y2 x3 y3 x4 y4 (meters)\n");
                        goto zone_done;
                    }
                }

                for (int i = 0; i < 4; i++) {
                    float xm = strtof(coords[i*2 + 0], NULL);
                    float ym = strtof(coords[i*2 + 1], NULL);
                    z.v[i].x_mm = m_to_mm(xm);
                    z.v[i].y_mm = m_to_mm(ym);
                }

                if (ld2450_set_zone((size_t)zi, &z) == ESP_OK) {
                    esp_err_t err = nvs_config_save_zone((uint8_t)zi, &z);
                    if (err == ESP_OK) {
                        printf("zone%d set (saved)\n", zi + 1);
                    } else {
                        printf("zone%d set BUT NVS SAVE FAILED: %s\n", zi + 1, esp_err_to_name(err));
                    }
                } else {
                    printf("zone%d update failed\n", zi + 1);
                }

zone_done:
                continue;
            }

            if (strcmp(cmd, "nvs") == 0) {
                printf("=== NVS Health Check ===\n");

                /* Get NVS stats */
                nvs_stats_t nvs_stats;
                esp_err_t err = nvs_get_stats(NULL, &nvs_stats);
                if (err == ESP_OK) {
                    printf("NVS partition stats:\n");
                    printf("  Used entries:  %zu\n", nvs_stats.used_entries);
                    printf("  Free entries:  %zu\n", nvs_stats.free_entries);
                    printf("  Total entries: %zu\n", nvs_stats.total_entries);
                    printf("  Namespace count: %zu\n", nvs_stats.namespace_count);
                } else {
                    printf("Failed to get NVS stats: %s\n", esp_err_to_name(err));
                }

                /* Test write/read */
                printf("\nTesting NVS write/read...\n");
                nvs_handle_t h;
                err = nvs_open("ld2450_cfg", NVS_READWRITE, &h);
                if (err != ESP_OK) {
                    printf("  nvs_open FAILED: %s\n", esp_err_to_name(err));
                    continue;
                }

                uint32_t test_val = 0xDEADBEEF;
                err = nvs_set_u32(h, "nvs_test", test_val);
                if (err != ESP_OK) {
                    printf("  nvs_set_u32 FAILED: %s\n", esp_err_to_name(err));
                    nvs_close(h);
                    continue;
                }

                err = nvs_commit(h);
                if (err != ESP_OK) {
                    printf("  nvs_commit FAILED: %s\n", esp_err_to_name(err));
                    nvs_close(h);
                    continue;
                }

                uint32_t read_val = 0;
                err = nvs_get_u32(h, "nvs_test", &read_val);
                nvs_close(h);

                if (err != ESP_OK) {
                    printf("  nvs_get_u32 FAILED: %s\n", esp_err_to_name(err));
                } else if (read_val != test_val) {
                    printf("  Data mismatch! Wrote 0x%08X, read 0x%08X\n", (unsigned int)test_val, (unsigned int)read_val);
                    printf("  NVS CORRUPTION DETECTED!\n");
                } else {
                    printf("  Write/read test PASSED (0x%08X)\n", (unsigned int)test_val);
                }

                continue;
            }

            if (strcmp(cmd, "factory-reset") == 0) {
                printf("FULL FACTORY RESET: Erasing Zigbee network + NVS config...\n");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(100));
                zigbee_full_factory_reset();
            }

            if (strcmp(cmd, "reboot") == 0) {
                printf("Rebooting...\n");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }

            printf("unknown command\n");
            print_help();
            continue;
        }

        // backspace/delete
        if (ch == 0x7f || ch == 0x08) {
            if (len > 0) len--;
            continue;
        }

        if (isprint((unsigned char)ch) && len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        }
    }
}

void ld2450_cli_start(void)
{
    const uart_port_t console_uart = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

    esp_err_t err = uart_driver_install(console_uart, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install(console_uart=%d) failed: %s", (int)console_uart, esp_err_to_name(err));
        return;
    }

    BaseType_t ok = xTaskCreate(cli_task, "ld2450_cli", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start CLI task");
    }
}
