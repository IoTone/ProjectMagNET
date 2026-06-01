/*
 * M5Stamp DigDug BLE Lighting
 *
 * MIT License
 *
 * Copyright (c) 2026 IoTone, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * BLE-controlled smart light on M5StampC3U (ESP32-C3).
 * SK6812 NeoPixel on GPIO 2, button on GPIO 9.
 * BLE GATT server for light control, WiFi for HTTP status/control,
 * Forth REPL for interactive configuration.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "led_strip.h"

/* BLE — NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Shared components */
#include "craw_serial.h"
#include "craw_wifi.h"
#include "craw_nvs.h"

/* Forth */
#include "forth_core.h"
#include "forth_version.h"

/* ---- Constants ---- */
#define LED_GPIO        2
#define BTN_GPIO        9
#define FORTH_HEAP      (32 * 1024)
#define LIGHT_NVS_NS    "digdug_light"
#define HOSTNAME_PREFIX  "DigDug"
#define BTN_LONG_PRESS_MS 5000
#define NVS_SAVE_DELAY_MS 2000
#define APP_VERSION      "0.1.0"

static const char *TAG = "digdug";

/* ---- Light State ---- */
typedef struct {
    uint8_t  power;       /* 0=off, 1=on */
    uint8_t  brightness;  /* 0-255 */
    uint8_t  r, g, b;     /* Color */
    uint16_t ramp_ms;     /* Transition time (0-5000) */
} light_state_t;

static light_state_t light = {0, 200, 255, 255, 255, 0};

/* ---- Hardware State ---- */
static led_strip_handle_t led_strip = NULL;
static volatile int btn_released = 0;
static volatile bool btn_held = false;
static int64_t btn_press_start = 0;

/* ---- Network State ---- */
static char hostname[32] = {0};
static char ip_address[20] = "N/A";
static httpd_handle_t http_server = NULL;

/* ---- Performance Debug ---- */
static bool debug_perf = false;  /* toggled via debug-on / debug-off Forth words */

#define PERF_START()    int64_t _perf_t0 = debug_perf ? esp_timer_get_time() : 0
#define PERF_END(label) do { \
    if (debug_perf) { \
        int64_t _dt = esp_timer_get_time() - _perf_t0; \
        ESP_LOGI(TAG, "[PERF] %s: %lld us (%.2f ms)", (label), _dt, _dt / 1000.0); \
    } \
} while(0)

/* ---- NVS Save Timer ---- */
static esp_timer_handle_t nvs_save_timer = NULL;

/* ---- BLE Characteristic Handles ---- */
static uint16_t power_handle;
static uint16_t brightness_handle;
static uint16_t color_r_handle;
static uint16_t color_g_handle;
static uint16_t color_b_handle;
static uint16_t ramp_handle;
static uint16_t ip_handle;
static uint16_t name_handle;

/* ---- Forward Declarations ---- */
static void apply_light_state(void);
static void light_state_changed(void);
static void ble_start_advertising(void);
static void start_http_server(void);

/* ---- LED Strip Driver ---- */

static void led_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}

static void apply_light_state(void) {
    PERF_START();
    if (light.power) {
        uint8_t ar = (uint8_t)((light.r * light.brightness) / 255);
        uint8_t ag = (uint8_t)((light.g * light.brightness) / 255);
        uint8_t ab = (uint8_t)((light.b * light.brightness) / 255);
        set_led(ar, ag, ab);
    } else {
        set_led(0, 0, 0);
    }
    PERF_END("apply_light_state");
}

/* ---- NVS for Light Settings ---- */

static void light_nvs_save_cb(void *arg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(LIGHT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_u8(h, "power", light.power);
    nvs_set_u8(h, "bright", light.brightness);
    nvs_set_u8(h, "r", light.r);
    nvs_set_u8(h, "g", light.g);
    nvs_set_u8(h, "b", light.b);
    nvs_set_u16(h, "ramp", light.ramp_ms);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Light state saved to NVS");
}

static void light_nvs_load(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(LIGHT_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved light state, using defaults");
        return;
    }
    nvs_get_u8(h, "power", &light.power);
    nvs_get_u8(h, "bright", &light.brightness);
    nvs_get_u8(h, "r", &light.r);
    nvs_get_u8(h, "g", &light.g);
    nvs_get_u8(h, "b", &light.b);
    nvs_get_u16(h, "ramp", &light.ramp_ms);
    nvs_close(h);
    ESP_LOGI(TAG, "Light state loaded: power=%d bright=%d r=%d g=%d b=%d ramp=%d",
             light.power, light.brightness, light.r, light.g, light.b, light.ramp_ms);
}

static void light_nvs_schedule_save(void) {
    /* Reset the debounce timer — save after NVS_SAVE_DELAY_MS of inactivity */
    esp_timer_stop(nvs_save_timer);  /* OK if not running */
    esp_timer_start_once(nvs_save_timer, NVS_SAVE_DELAY_MS * 1000);
}

static void light_state_changed(void) {
    PERF_START();
    apply_light_state();
    light_nvs_schedule_save();
    PERF_END("light_state_changed (total)");
}

/* ---- Button ISR ---- */

static void IRAM_ATTR btn_isr_handler(void *arg) {
    if (gpio_get_level(BTN_GPIO) == 1) {
        btn_released = 1;
    }
}

static void btn_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_GPIO, btn_isr_handler, NULL);
}

/* ---- Button Task ---- */

static void btn_task(void *arg) {
    while (1) {
        /* Long-press detection: check if button is still held */
        if (gpio_get_level(BTN_GPIO) == 0) {
            if (!btn_held) {
                btn_held = true;
                btn_press_start = esp_timer_get_time();
            } else {
                int64_t held_ms = (esp_timer_get_time() - btn_press_start) / 1000;
                if (held_ms > BTN_LONG_PRESS_MS) {
                    craw_serial_print("\r\n[BTN] Long press -- clearing WiFi and rebooting...\r\n");
                    char ssid[33], pass[65];
                    craw_nvs_clear_wifi_creds(ssid, pass);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
        } else {
            btn_held = false;
        }

        /* Short press: toggle power */
        if (btn_released) {
            btn_released = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BTN_GPIO) == 1) {
                light.power = light.power ? 0 : 1;
                craw_serial_printf("\r\n[BTN] Power %s\r\n", light.power ? "ON" : "OFF");
                light_state_changed();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- Hostname ---- */

static void derive_hostname(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "%s-%02X%02X", HOSTNAME_PREFIX, mac[4], mac[5]);
}

/* ---- BLE GATT Server ---- */

/* Service UUID: dd841d1c-869e-48b6-b882-924fd3276600 */
static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0x00, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);

/* Characteristic UUIDs — last 2 bytes differ: ...01 through ...08 */
static const ble_uuid128_t chr_power_uuid =
    BLE_UUID128_INIT(0x01, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);
static const ble_uuid128_t chr_brightness_uuid =
    BLE_UUID128_INIT(0x02, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);
static const ble_uuid128_t chr_r_uuid =
    BLE_UUID128_INIT(0x03, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);
static const ble_uuid128_t chr_g_uuid =
    BLE_UUID128_INIT(0x04, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);
static const ble_uuid128_t chr_b_uuid =
    BLE_UUID128_INIT(0x05, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);
static const ble_uuid128_t chr_ramp_uuid =
    BLE_UUID128_INIT(0x06, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);
static const ble_uuid128_t chr_ip_uuid =
    BLE_UUID128_INIT(0x07, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);
static const ble_uuid128_t chr_name_uuid =
    BLE_UUID128_INIT(0x08, 0x66, 0x27, 0xd3, 0x4f, 0x92, 0x82, 0xb8,
                     0xb6, 0x48, 0x9e, 0x86, 0x1c, 0x1d, 0x84, 0xdd);

/* ---- BLE Access Callbacks ---- */

static int chr_access_power(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &light.power, sizeof(light.power));
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &light.power, 1, NULL);
            ESP_LOGI(TAG, "BLE write power=%d", light.power);
            light_state_changed();
        }
    }
    return 0;
}

static int chr_access_brightness(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &light.brightness, sizeof(light.brightness));
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &light.brightness, 1, NULL);
            ESP_LOGI(TAG, "BLE write brightness=%d", light.brightness);
            light_state_changed();
        }
    }
    return 0;
}

static int chr_access_color_r(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &light.r, sizeof(light.r));
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &light.r, 1, NULL);
            ESP_LOGI(TAG, "BLE write r=%d", light.r);
            light_state_changed();
        }
    }
    return 0;
}

static int chr_access_color_g(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &light.g, sizeof(light.g));
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &light.g, 1, NULL);
            ESP_LOGI(TAG, "BLE write g=%d", light.g);
            light_state_changed();
        }
    }
    return 0;
}

static int chr_access_color_b(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &light.b, sizeof(light.b));
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &light.b, 1, NULL);
            ESP_LOGI(TAG, "BLE write b=%d", light.b);
            light_state_changed();
        }
    }
    return 0;
}

static int chr_access_ramp(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &light.ramp_ms, sizeof(light.ramp_ms));
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 2) {
            ble_hs_mbuf_to_flat(ctxt->om, &light.ramp_ms, 2, NULL);
            if (light.ramp_ms > 5000) light.ramp_ms = 5000;
            ESP_LOGI(TAG, "BLE write ramp=%d", light.ramp_ms);
            light_state_changed();
        }
    }
    return 0;
}

static int chr_access_ip(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, ip_address, strlen(ip_address));
    }
    return 0;
}

static int chr_access_name(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, hostname, strlen(hostname));
    }
    return 0;
}

/* ---- GATT Service Definition ---- */

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   /* Power */
                .uuid = &chr_power_uuid.u,
                .access_cb = chr_access_power,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &power_handle,
            },
            {   /* Brightness */
                .uuid = &chr_brightness_uuid.u,
                .access_cb = chr_access_brightness,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &brightness_handle,
            },
            {   /* Color R */
                .uuid = &chr_r_uuid.u,
                .access_cb = chr_access_color_r,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &color_r_handle,
            },
            {   /* Color G */
                .uuid = &chr_g_uuid.u,
                .access_cb = chr_access_color_g,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &color_g_handle,
            },
            {   /* Color B */
                .uuid = &chr_b_uuid.u,
                .access_cb = chr_access_color_b,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &color_b_handle,
            },
            {   /* Ramp Rate */
                .uuid = &chr_ramp_uuid.u,
                .access_cb = chr_access_ramp,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &ramp_handle,
            },
            {   /* IP Address */
                .uuid = &chr_ip_uuid.u,
                .access_cb = chr_access_ip,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &ip_handle,
            },
            {   /* Device Name */
                .uuid = &chr_name_uuid.u,
                .access_cb = chr_access_name,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &name_handle,
            },
            { 0 },  /* Terminator */
        },
    },
    { 0 },  /* Terminator */
};

/* ---- GAP Event Handler ---- */

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "BLE connect: status=%d, handle=%d",
                     event->connect.status, event->connect.conn_handle);
            if (event->connect.status != 0) {
                ble_start_advertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE disconnect: reason=%d",
                     event->disconnect.reason);
            ble_start_advertising();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG, "BLE adv complete");
            ble_start_advertising();
            break;
        default:
            break;
    }
    return 0;
}

/* ---- BLE Advertising ---- */

static void ble_start_advertising(void) {
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)hostname;
    fields.name_len = strlen(hostname);
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, gap_event_handler, NULL);
    ESP_LOGI(TAG, "BLE advertising started as '%s'", hostname);
}

/* ---- BLE Init ---- */

static void ble_host_task(void *param) {
    nimble_port_run();  /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void) {
    ESP_LOGI(TAG, "BLE host synced");
    ble_start_advertising();
}

static void ble_init(void) {
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_device_name_set(hostname);

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE initialized");
}

/* ---- HTTP Server ---- */

static esp_err_t handler_light_status(httpd_req_t *req) {
    PERF_START();
    char json[400];
    snprintf(json, sizeof(json),
        "{\"power\":%d,\"brightness\":%d,"
        "\"r\":%d,\"g\":%d,\"b\":%d,"
        "\"ramp_ms\":%d,"
        "\"ip\":\"%s\","
        "\"hostname\":\"%s\","
        "\"wifi_connected\":%s,"
        "\"ble_active\":true,"
        "\"uptime_s\":%lu,"
        "\"free_heap\":%lu}",
        light.power, light.brightness,
        light.r, light.g, light.b,
        light.ramp_ms,
        ip_address,
        hostname,
        craw_wifi_is_connected() ? "true" : "false",
        (unsigned long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    PERF_END("HTTP /light/status");
    return ESP_OK;
}

static esp_err_t handler_light_set(httpd_req_t *req) {
    PERF_START();
    char query[128] = {0};
    char param[8] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "power", param, sizeof(param)) == ESP_OK) {
            light.power = (uint8_t)atoi(param);
        }
        if (httpd_query_key_value(query, "brightness", param, sizeof(param)) == ESP_OK) {
            light.brightness = (uint8_t)atoi(param);
        }
        if (httpd_query_key_value(query, "r", param, sizeof(param)) == ESP_OK) {
            light.r = (uint8_t)atoi(param);
        }
        if (httpd_query_key_value(query, "g", param, sizeof(param)) == ESP_OK) {
            light.g = (uint8_t)atoi(param);
        }
        if (httpd_query_key_value(query, "b", param, sizeof(param)) == ESP_OK) {
            light.b = (uint8_t)atoi(param);
        }
        if (httpd_query_key_value(query, "ramp_ms", param, sizeof(param)) == ESP_OK) {
            light.ramp_ms = (uint16_t)atoi(param);
            if (light.ramp_ms > 5000) light.ramp_ms = 5000;
        }
        light_state_changed();
        ESP_LOGI(TAG, "HTTP set: power=%d bright=%d r=%d g=%d b=%d ramp=%d",
                 light.power, light.brightness, light.r, light.g, light.b, light.ramp_ms);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    PERF_END("HTTP /light/set");
    return ESP_OK;
}

static esp_err_t handler_ping(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "pong");
    return ESP_OK;
}

static void start_http_server(void) {
    if (http_server) return;  /* Already running */

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;

    if (httpd_start(&http_server, &config) == ESP_OK) {
        httpd_uri_t uri_status = {
            .uri = "/light/status", .method = HTTP_GET,
            .handler = handler_light_status, .user_ctx = NULL
        };
        httpd_uri_t uri_set = {
            .uri = "/light/set", .method = HTTP_GET,
            .handler = handler_light_set, .user_ctx = NULL
        };
        httpd_uri_t uri_ping = {
            .uri = "/ping", .method = HTTP_GET,
            .handler = handler_ping, .user_ctx = NULL
        };
        httpd_register_uri_handler(http_server, &uri_status);
        httpd_register_uri_handler(http_server, &uri_set);
        httpd_register_uri_handler(http_server, &uri_ping);
        ESP_LOGI(TAG, "HTTP server started on port 80");
    }
}

/* ---- WiFi Event Callback ---- */

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    switch (event) {
        case CRAW_WIFI_EVENT_CONNECTED:
            craw_wifi_get_ip_str(ip_address, sizeof(ip_address));
            ESP_LOGI(TAG, "WiFi connected, IP: %s", ip_address);
            craw_serial_printf("\r\n[WiFi] Connected, IP: %s\r\n", ip_address);
            start_http_server();
            /* Notify BLE clients of IP change */
            ble_gatts_chr_updated(ip_handle);
            break;
        case CRAW_WIFI_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected");
            craw_serial_print("\r\n[WiFi] Disconnected\r\n");
            snprintf(ip_address, sizeof(ip_address), "N/A");
            ble_gatts_chr_updated(ip_handle);
            break;
        case CRAW_WIFI_EVENT_CONNECT_FAILED:
            ESP_LOGE(TAG, "WiFi connect failed");
            craw_serial_print("\r\n[WiFi] Connect failed\r\n");
            snprintf(ip_address, sizeof(ip_address), "N/A");
            break;
    }
}

/* ---- Forth Words ---- */

/* LED words */

static void w_led_on(void) {
    light.power = 1;
    light_state_changed();
    craw_serial_print("LED ON\r\n");
}

static void w_led_off(void) {
    light.power = 0;
    light_state_changed();
    craw_serial_print("LED OFF\r\n");
}

static void w_led_rgb(void) {
    int b = (int)forth_pop();
    int g = (int)forth_pop();
    int r = (int)forth_pop();
    light.r = (uint8_t)r;
    light.g = (uint8_t)g;
    light.b = (uint8_t)b;
    light.power = 1;
    light_state_changed();
    craw_serial_printf("LED: r=%d g=%d b=%d\r\n", light.r, light.g, light.b);
}

static void w_led_brightness(void) {
    int n = (int)forth_pop();
    light.brightness = (uint8_t)(n & 0xFF);
    light_state_changed();
    craw_serial_printf("Brightness: %d\r\n", light.brightness);
}

static void w_led_ramp(void) {
    int n = (int)forth_pop();
    if (n < 0) n = 0;
    if (n > 5000) n = 5000;
    light.ramp_ms = (uint16_t)n;
    light_state_changed();
    craw_serial_printf("Ramp: %d ms\r\n", light.ramp_ms);
}

static void w_led_status(void) {
    craw_serial_printf("Power:      %s\r\n", light.power ? "ON" : "OFF");
    craw_serial_printf("Brightness: %d\r\n", light.brightness);
    craw_serial_printf("Color:      r=%d g=%d b=%d\r\n", light.r, light.g, light.b);
    craw_serial_printf("Ramp:       %d ms\r\n", light.ramp_ms);
}

/* BLE words */

static void w_ble_status(void) {
    craw_serial_printf("BLE Name:   %s\r\n", hostname);
    craw_serial_print("BLE Active: yes\r\n");
}

static void w_ble_advertise(void) {
    ble_start_advertising();
    craw_serial_print("BLE advertising restarted.\r\n");
}

/* WiFi words */

static void w_wifi_ssid(void) {
    craw_serial_print("SSID: ");
    char ssid[33] = {0};
    craw_serial_read_line(ssid, sizeof(ssid), true, false);
    /* Save via NVS profile system — save to active profile */
    char pass[65] = {0};
    char old_ssid[33], old_pass[65];
    if (craw_nvs_load_wifi_creds(old_ssid, old_pass)) {
        strncpy(pass, old_pass, sizeof(pass) - 1);
    }
    craw_nvs_save_wifi_creds(ssid, pass);
    craw_serial_printf("SSID set to: '%s'\r\n", ssid);
}

static void w_wifi_pass(void) {
    craw_serial_print("Password (Enter for open): ");
    char pass[65] = {0};
    craw_serial_read_line(pass, sizeof(pass), false, true);
    /* Load current SSID, then save with new password */
    char ssid[33], old_pass[65];
    if (craw_nvs_load_wifi_creds(ssid, old_pass)) {
        craw_nvs_save_wifi_creds(ssid, pass);
    } else {
        craw_serial_print("No SSID set. Use wifi-ssid first.\r\n");
        return;
    }
    if (strlen(pass) == 0)
        craw_serial_print("No password (open network)\r\n");
    else
        craw_serial_print("Password set.\r\n");
}

static void w_wifi_connect(void) {
    char ssid[33], pass[65];
    if (!craw_nvs_load_wifi_creds(ssid, pass)) {
        craw_serial_print("No SSID configured. Use wifi-ssid first.\r\n");
        return;
    }
    craw_serial_printf("Connecting to '%s'...\r\n", ssid);
    craw_wifi_connect(ssid, pass);
}

static void w_wifi_status(void) {
    char ssid[33], pass[65];
    bool has_creds = craw_nvs_load_wifi_creds(ssid, pass);
    craw_serial_printf("SSID:      '%s'\r\n", has_creds ? ssid : "(none)");
    craw_serial_printf("Connected: %s\r\n", craw_wifi_is_connected() ? "yes" : "no");
    craw_serial_printf("IP:        %s\r\n", ip_address);
    craw_serial_printf("Hostname:  %s\r\n", hostname);
}

static void w_wifi_clear(void) {
    char ssid[33], pass[65];
    craw_nvs_clear_wifi_creds(ssid, pass);
    craw_wifi_disconnect();
    snprintf(ip_address, sizeof(ip_address), "N/A");
    craw_serial_print("WiFi credentials cleared.\r\n");
}

/* System words */

static void w_versions(void) {
    craw_serial_printf("App:         %s\r\n", APP_VERSION);
    craw_serial_printf("craw_serial: %s\r\n", CRAW_SERIAL_VERSION);
    craw_serial_printf("craw_wifi:   %s\r\n", CRAW_WIFI_VERSION);
    craw_serial_printf("craw_nvs:    %s\r\n", CRAW_NVS_VERSION);
    craw_serial_printf("ESPIDFORTH:  %s\r\n", ESPIDFORTH_VERSION_STRING);
    craw_serial_printf("ESP-IDF:     %s\r\n", esp_get_idf_version());
}

static void w_status(void) {
    w_led_status();
    craw_serial_print("---\r\n");
    w_ble_status();
    craw_serial_print("---\r\n");
    w_wifi_status();
    craw_serial_print("---\r\n");
    craw_serial_printf("Free heap:   %lu bytes\r\n", (unsigned long)esp_get_free_heap_size());
    craw_serial_printf("Uptime:      %lu s\r\n",
                       (unsigned long)(esp_timer_get_time() / 1000000));
}

static void w_save(void) {
    light_nvs_save_cb(NULL);
    craw_serial_print("Light state saved to NVS.\r\n");
}

/* ---- Debug Forth Words ---- */

static void w_debug_on(void) {
    debug_perf = true;
    craw_serial_print("Performance timing ON\r\n");
}

static void w_debug_off(void) {
    debug_perf = false;
    craw_serial_print("Performance timing OFF\r\n");
}

/* ---- Timing Forth Words ---- */

static void w_ms(void) {
    int ms = forth_pop();
    if (ms > 0 && ms < 30000) vTaskDelay(pdMS_TO_TICKS(ms));
}

static void w_millis(void) {
    forth_push((intptr_t)(esp_timer_get_time() / 1000));
}

/* ---- Register All Forth Words ---- */

static void register_forth_words(void) {
    /* LED */
    forth_register_word("led-on", w_led_on);
    forth_register_word("led-off", w_led_off);
    forth_register_word("led-rgb", w_led_rgb);
    forth_register_word("led-brightness", w_led_brightness);
    forth_register_word("led-ramp", w_led_ramp);
    forth_register_word("led-status", w_led_status);

    /* BLE */
    forth_register_word("ble-status", w_ble_status);
    forth_register_word("ble-advertise", w_ble_advertise);

    /* WiFi */
    forth_register_word("wifi-ssid", w_wifi_ssid);
    forth_register_word("wifi-pass", w_wifi_pass);
    forth_register_word("wifi-connect", w_wifi_connect);
    forth_register_word("wifi-status", w_wifi_status);
    forth_register_word("wifi-clear", w_wifi_clear);

    /* Timing */
    forth_register_word("ms", w_ms);
    forth_register_word("millis", w_millis);

    /* Debug */
    forth_register_word("debug-on", w_debug_on);
    forth_register_word("debug-off", w_debug_off);

    /* System */
    forth_register_word("versions", w_versions);
    forth_register_word("status", w_status);
    forth_register_word("save", w_save);
}

/* ---- Boot Splash ---- */

static void print_splash(void) {
    craw_serial_print("\r\n\r\n");
    craw_serial_printf(
        "\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x97"
        "\r\n");
    craw_serial_printf(
        "\xe2\x95\x91  M5Stamp DigDug BLE Lighting v%s \xe2\x95\x91\r\n", APP_VERSION);
    craw_serial_print(
        "\xe2\x95\x91  IoTone, Inc. 2026                   \xe2\x95\x91\r\n");
    craw_serial_print(
        "\xe2\x95\xa0\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa3\r\n");
    craw_serial_printf(
        "\xe2\x95\x91  craw_serial  v%-22s\xe2\x95\x91\r\n", CRAW_SERIAL_VERSION);
    craw_serial_printf(
        "\xe2\x95\x91  craw_wifi    v%-22s\xe2\x95\x91\r\n", CRAW_WIFI_VERSION);
    craw_serial_printf(
        "\xe2\x95\x91  craw_nvs     v%-22s\xe2\x95\x91\r\n", CRAW_NVS_VERSION);
    craw_serial_printf(
        "\xe2\x95\x91  ESPIDFORTH   v%-22s\xe2\x95\x91\r\n", ESPIDFORTH_VERSION_STRING);
    craw_serial_printf(
        "\xe2\x95\x91  ESP-IDF      %-23s\xe2\x95\x91\r\n", esp_get_idf_version());
    craw_serial_print(
        "\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
        "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d\r\n");

    craw_serial_printf("Board:   M5StampC3U (ESP32-C3)\r\n");
    craw_serial_printf("LED:     SK6812 on GPIO %d\r\n", LED_GPIO);
    craw_serial_printf("Button:  GPIO %d (active low)\r\n", BTN_GPIO);
    craw_serial_print("BLE:     NimBLE GATT server\r\n");
}

/* ---- Main ---- */

void app_main(void) {
    /* 1. Serial */
    craw_serial_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 2. Boot splash */
    print_splash();

    /* 3. LED */
    led_init();

    /* 4. Button */
    btn_init();

    /* 5. NVS */
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    light_nvs_load();
    apply_light_state();

    /* 6. NVS debounce timer */
    const esp_timer_create_args_t timer_args = {
        .callback = light_nvs_save_cb,
        .arg = NULL,
        .name = "nvs_save",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &nvs_save_timer));

    /* 7. Hostname */
    derive_hostname();
    craw_serial_printf("Hostname: %s\r\n", hostname);

    /* 8. WiFi */
    craw_wifi_init(hostname, on_wifi_event, NULL);

    /* 9. BLE */
    ble_init();

    /* 10. Forth */
    forth_init(FORTH_HEAP);
    register_forth_words();
    craw_serial_printf("Forth engine initialized. Free heap: %lu bytes\r\n",
                       (unsigned long)esp_get_free_heap_size());

    /* 11. Button task */
    xTaskCreate(btn_task, "btn_task", 3072, NULL, 5, NULL);

    /* 12. Auto-connect WiFi */
    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass)) {
        craw_serial_printf("WiFi: connecting to '%s'...\r\n", ssid);
        craw_wifi_connect(ssid, pass);
    } else {
        craw_serial_print("No stored WiFi. Use wifi-ssid / wifi-pass / wifi-connect.\r\n");
    }

    /* 13. Help */
    craw_serial_print("\r\nType 'words' for commands.\r\n");
    craw_serial_print("  led-on / led-off / R G B led-rgb / N led-brightness\r\n");
    craw_serial_print("  led-status / ble-status / wifi-status / status\r\n");
    craw_serial_print("  Hold button 5s to clear WiFi + reboot.\r\n\r\n");

    /* 14. Forth REPL (blocks forever) */
    forth_repl(craw_serial_getchar, craw_serial_putchar);

    forth_deinit();
}
