/*
 * Claw OpenWR — Claude Code LED Notifier
 *
 * M5Stamp C3U receives HTTP notifications from Claude Code hooks
 * and displays status as LED patterns on the onboard SK6812 NeoPixel.
 *
 * Modes:
 *   0: Off (idle)
 *   1: Random Flash (manual)
 *   2: Slow Breathe (manual)
 *   3: Attention — fast strobe (Claude needs input)
 *   4: Rainbow (manual)
 *   5: Task Complete — green pulse (Claude finished)
 *   6: Network Offline — solid pink
 *   7: Error — red strobe (Claude error)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "led_strip.h"
#include "forth_core.h"
#include "forth_version.h"

/* ---- Hardware Pins ---- */
#define LED_GPIO        2
#define BTN_GPIO        9

/* ---- Config ---- */
#define FORTH_HEAP_SIZE     (48 * 1024)
#define NUM_MODES           8
#define NVS_NAMESPACE       "claw_wifi"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "pass"
#define HOSTNAME_PREFIX     "ClawOpenWR"
#define WIFI_MAX_RETRY      5
#define BTN_LONG_PRESS_MS   5000
#define NVS_KEY_MQTT_BROKER "mqtt_url"
#define MQTT_DEFAULT_BROKER "mqtt://broker.hivemq.com:1883"

static const char *TAG = "claw";

/* ---- State ---- */
static led_strip_handle_t led_strip = NULL;
static volatile int current_mode = 0;
static volatile int btn_released = 0;

/* WiFi */
static char wifi_ssid[33] = {0};
static char wifi_pass[65] = {0};
static char hostname[32] = {0};
static char hostname_mdns[32] = {0};
static volatile bool wifi_connected = false;
static esp_netif_t *sta_netif = NULL;
static httpd_handle_t http_server = NULL;
static int wifi_retry_count = 0;

/* MQTT */
static char mqtt_broker_uri[128] = MQTT_DEFAULT_BROKER;
static char mqtt_topic[80] = {0};
static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool mqtt_connected = false;

/* Green strobe override (5s on connect) */
static volatile bool strobe_override = false;
static int64_t strobe_override_until = 0;

/* Button long-press tracking */
static volatile bool btn_held = false;
static int64_t btn_press_start = 0;

/* ---- USB Serial I/O ---- */

static void usb_print(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}

static void usb_printf(const char *fmt, ...) {
    char buf[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    usb_print(buf);
}

static int uart_getchar(void) {
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    if (n <= 0) return -1;
    return c;
}

static void uart_putchar(int c) {
    uint8_t ch = (uint8_t)c;
    usb_serial_jtag_write_bytes(&ch, 1, pdMS_TO_TICKS(100));
}

static void setup_usb_serial(void) {
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&cfg);
}

/* Read a line interactively from USB serial.
 * echo: true = show chars, false = show '*'
 * allow_empty: true = Enter on empty line returns "", false = ignores empty Enter */
static void read_line_from_serial(char *buf, int maxlen, bool echo, bool allow_empty) {
    /* Drain any leftover \r\n from the previous Enter key */
    vTaskDelay(pdMS_TO_TICKS(50));
    while (1) {
        int drain = uart_getchar();
        if (drain < 0) break;
    }

    bool first_enter_skipped = false;
    int pos = 0;
    while (pos < maxlen - 1) {
        int ch = uart_getchar();
        if (ch < 0) continue;
        if (ch == '\r' || ch == '\n') {
            /* Skip the very first Enter (likely leftover from drain timing) */
            if (!first_enter_skipped) { first_enter_skipped = true; continue; }
            if (pos == 0 && !allow_empty) continue;
            break;
        }
        first_enter_skipped = true;
        if (ch == 8 || ch == 127) {
            if (pos > 0) { pos--; if (echo) usb_print("\b \b"); }
            continue;
        }
        if (ch >= 32) {
            buf[pos++] = (char)ch;
            if (echo) uart_putchar(ch); else uart_putchar('*');
        }
    }
    buf[pos] = '\0';
    usb_print("\r\n");
}

/* ---- Millis helper ---- */
static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- LED Strip ---- */

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

/* ---- Mode Names ---- */

static const char *mode_name(int mode) {
    switch (mode) {
        case 0: return "Off";
        case 1: return "Random Flash";
        case 2: return "Slow Breathe";
        case 3: return "Attention (Fast Strobe)";
        case 4: return "Rainbow";
        case 5: return "Task Complete (Green Pulse)";
        case 6: return "Network Offline (Pink)";
        case 7: return "Error (Red Strobe)";
        default: return "Unknown";
    }
}

static void set_mode(int mode) {
    if (mode < 0 || mode >= NUM_MODES) mode = 0;
    current_mode = mode;
    usb_printf("\r\nMode %d: %s\r\n", current_mode, mode_name(current_mode));
}

/* ---- Button Check (called from LED task) ---- */

static bool check_button(void) {
    /* Long-press detection: check if button is still held */
    if (gpio_get_level(BTN_GPIO) == 0) {
        if (!btn_held) {
            btn_held = true;
            btn_press_start = esp_timer_get_time();
        } else {
            int64_t held_ms = (esp_timer_get_time() - btn_press_start) / 1000;
            if (held_ms > BTN_LONG_PRESS_MS) {
                /* Long press: clear WiFi creds and reboot */
                usb_print("\r\n[BTN] Long press — clearing WiFi and rebooting...\r\n");
                nvs_handle_t h;
                if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_all(h);
                    nvs_commit(h);
                    nvs_close(h);
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
    } else {
        btn_held = false;
    }

    /* Short press: cycle manual modes 0-4 */
    if (btn_released) {
        btn_released = 0;
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(BTN_GPIO) == 1) {
            int next = (current_mode + 1) % 5;  /* Only modes 0-4 */
            set_mode(next);
            return true;
        }
    }
    return false;
}

/* ---- HSV to RGB ---- */

static void hsv_to_rgb(int h, int s, int v, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (s == 0) { *r = *g = *b = v; return; }
    int region = h / 60;
    int remainder = (h - (region * 60)) * 255 / 60;
    int p = (v * (255 - s)) >> 8;
    int q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    int t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

/* ---- Pattern Functions ---- */

static void pattern_off(void) {
    set_led(0, 0, 0);
    while (current_mode == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        check_button();
    }
}

static void pattern_random_flash(void) {
    while (current_mode == 1) {
        uint32_t rnd = esp_random();
        set_led((rnd >> 16) & 0xFF, (rnd >> 8) & 0xFF, rnd & 0xFF);
        for (int i = 0; i < 20 && current_mode == 1; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
    }
}

static void pattern_breathe(void) {
    int hue = esp_random() % 360;
    while (current_mode == 2) {
        for (int v = 0; v <= 255 && current_mode == 2; v += 3) {
            uint8_t r, g, b;
            hsv_to_rgb(hue, 255, v, &r, &g, &b);
            set_led(r, g, b);
            vTaskDelay(pdMS_TO_TICKS(12));
            check_button();
        }
        for (int v = 255; v >= 0 && current_mode == 2; v -= 3) {
            uint8_t r, g, b;
            hsv_to_rgb(hue, 255, v, &r, &g, &b);
            set_led(r, g, b);
            vTaskDelay(pdMS_TO_TICKS(12));
            check_button();
        }
        hue = esp_random() % 360;
    }
}

/* Mode 3: Attention — fast strobe random colors */
static void pattern_strobe(void) {
    while (current_mode == 3) {
        uint32_t rnd = esp_random();
        set_led((rnd >> 16) & 0xFF, (rnd >> 8) & 0xFF, rnd & 0xFF);
        for (int i = 0; i < 5 && current_mode == 3; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
        set_led(0, 0, 0);
        for (int i = 0; i < 5 && current_mode == 3; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
    }
}

/* Mode 4: Rainbow */
static void pattern_rainbow(void) {
    int hue = 0;
    while (current_mode == 4) {
        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, 180, &r, &g, &b);
        set_led(r, g, b);
        hue = (hue + 1) % 360;
        vTaskDelay(pdMS_TO_TICKS(5));
        check_button();
    }
}

/* Mode 5: Task Complete — green fade-in pulse, 1000ms cycle */
static void pattern_green_pulse(void) {
    while (current_mode == 5) {
        /* Fade in: 0 to 255 over ~500ms */
        for (int v = 0; v <= 255 && current_mode == 5; v += 5) {
            set_led(0, v, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
        /* Hold at full green ~200ms */
        for (int i = 0; i < 20 && current_mode == 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
        /* Fade out: 255 to 0 over ~300ms */
        for (int v = 255; v >= 0 && current_mode == 5; v -= 8) {
            set_led(0, v > 0 ? v : 0, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
        set_led(0, 0, 0);
        /* Brief pause before next cycle */
        for (int i = 0; i < 5 && current_mode == 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
    }
}

/* Mode 6: Network Offline — solid pink */
static void pattern_solid_pink(void) {
    set_led(255, 105, 180);
    while (current_mode == 6 && !strobe_override) {
        vTaskDelay(pdMS_TO_TICKS(50));
        check_button();
    }
}

/* Mode 7: Error — strobing red */
static void pattern_red_strobe(void) {
    while (current_mode == 7) {
        set_led(255, 0, 0);
        for (int i = 0; i < 10 && current_mode == 7; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
        set_led(0, 0, 0);
        for (int i = 0; i < 10 && current_mode == 7; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            check_button();
        }
    }
}

/* ---- LED Task ---- */

static void led_task(void *arg) {
    while (1) {
        /* Green strobe override (WiFi connect success, 5s) */
        if (strobe_override && esp_timer_get_time() < strobe_override_until) {
            set_led(0, 255, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            set_led(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (strobe_override) {
            strobe_override = false;
            current_mode = 0;
        }

        switch (current_mode) {
            case 0: pattern_off(); break;
            case 1: pattern_random_flash(); break;
            case 2: pattern_breathe(); break;
            case 3: pattern_strobe(); break;
            case 4: pattern_rainbow(); break;
            case 5: pattern_green_pulse(); break;
            case 6: pattern_solid_pink(); break;
            case 7: pattern_red_strobe(); break;
        }
    }
}

/* ---- NVS ---- */

static void nvs_init_flash(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static bool nvs_load_wifi_creds(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = sizeof(wifi_ssid);
    esp_err_t err = nvs_get_str(h, NVS_KEY_SSID, wifi_ssid, &len);
    if (err != ESP_OK) { nvs_close(h); return false; }

    len = sizeof(wifi_pass);
    err = nvs_get_str(h, NVS_KEY_PASS, wifi_pass, &len);
    if (err != ESP_OK) {
        wifi_pass[0] = '\0'; /* Open network, no password */
    }

    nvs_close(h);
    return (strlen(wifi_ssid) > 0);
}

static void nvs_save_wifi_creds(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_SSID, wifi_ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, NVS_KEY_PASS, wifi_pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

static void nvs_clear_wifi_creds(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    wifi_ssid[0] = '\0';
    wifi_pass[0] = '\0';
}

/* ---- Hostname ---- */

static void derive_hostname(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "%s_%02x%02x", HOSTNAME_PREFIX, mac[4], mac[5]);
    /* mDNS hostnames must be lowercase per RFC 6762 */
    snprintf(hostname_mdns, sizeof(hostname_mdns), "clawopenwr-%02x%02x", mac[4], mac[5]);
}

/* ---- HTTP Server ---- */

static esp_err_t handler_notify(httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[8] = {0};
        if (httpd_query_key_value(query, "state", param, sizeof(param)) == ESP_OK) {
            int state = atoi(param);
            if (state >= 0 && state < NUM_MODES) {
                current_mode = state;
                ESP_LOGI(TAG, "HTTP notify: mode %d (%s)", state, mode_name(state));
            }
        }
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t handler_status(httpd_req_t *req) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    esp_netif_ip_info_t ip_info = {0};
    if (sta_netif) esp_netif_get_ip_info(sta_netif, &ip_info);

    char json[300];
    snprintf(json, sizeof(json),
        "{\"mode\":%d,\"mode_name\":\"%s\",\"ip\":\"" IPSTR "\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"hostname\":\"%s.local\",\"uptime_s\":%lu,\"free_heap\":%lu}",
        current_mode, mode_name(current_mode),
        IP2STR(&ip_info.ip),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        hostname_mdns,
        (unsigned long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t handler_ping(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "pong");
    return ESP_OK;
}

static void start_http_server(void) {
    if (http_server) return; /* Already running */

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;

    if (httpd_start(&http_server, &config) == ESP_OK) {
        httpd_uri_t uri_notify = { .uri = "/notify", .method = HTTP_GET, .handler = handler_notify, .user_ctx = NULL };
        httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = handler_status, .user_ctx = NULL };
        httpd_uri_t uri_ping   = { .uri = "/ping",   .method = HTTP_GET, .handler = handler_ping,   .user_ctx = NULL };
        httpd_register_uri_handler(http_server, &uri_notify);
        httpd_register_uri_handler(http_server, &uri_status);
        httpd_register_uri_handler(http_server, &uri_ping);
        ESP_LOGI(TAG, "HTTP server started on port 80");
    }
}

/* ---- mDNS ---- */

static void start_mdns_service(void) {
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(hostname_mdns);
    mdns_instance_name_set("Claw OpenWR Notifier");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", hostname_mdns);
}

/* ---- MQTT ---- */

static void mqtt_event_handler(void *args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            esp_mqtt_client_subscribe(mqtt_client, mqtt_topic, 1);
            ESP_LOGI(TAG, "MQTT connected, subscribed to: %s", mqtt_topic);
            usb_printf("\r\n[MQTT] Connected, topic: %s\r\n", mqtt_topic);
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA:
            if (event->data_len > 0 && event->data_len < 4) {
                char buf[4] = {0};
                memcpy(buf, event->data, event->data_len);
                int state = atoi(buf);
                if (state >= 0 && state < NUM_MODES) {
                    current_mode = state;
                    ESP_LOGI(TAG, "MQTT: mode %d (%s)", state, mode_name(state));
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
        default:
            break;
    }
}

static void derive_mqtt_topic(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mqtt_topic, sizeof(mqtt_topic),
             "iotj/cl/openwr/updates/%02x%02x", mac[4], mac[5]);
}

static void start_mqtt(void) {
    if (mqtt_client) return;
    derive_mqtt_topic();

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = mqtt_broker_uri;

    mqtt_client = esp_mqtt_client_init(&cfg);
    if (!mqtt_client) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT started, broker: %s", mqtt_broker_uri);
}

static void nvs_load_mqtt_broker(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(mqtt_broker_uri);
        if (nvs_get_str(h, NVS_KEY_MQTT_BROKER, mqtt_broker_uri, &len) != ESP_OK) {
            strncpy(mqtt_broker_uri, MQTT_DEFAULT_BROKER, sizeof(mqtt_broker_uri) - 1);
        }
        nvs_close(h);
    }
}

static void nvs_save_mqtt_broker(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_MQTT_BROKER, mqtt_broker_uri);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- WiFi ---- */

static void wifi_retry_task(void *arg) {
    /* Runs in its own task so we don't block the event loop */
    while (wifi_retry_count < WIFI_MAX_RETRY && !wifi_connected) {
        wifi_retry_count++;
        usb_printf("\r\n[WiFi] Retry %d/%d...\r\n", wifi_retry_count, WIFI_MAX_RETRY);
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!wifi_connected) esp_wifi_connect();
        vTaskDelay(pdMS_TO_TICKS(3000)); /* Wait for result */
    }
    if (!wifi_connected) {
        usb_print("\r\n[WiFi] Connect failed.\r\n");
        current_mode = 6;
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi disconnected, reason=%d", disc->reason);
            usb_printf("\r\n[WiFi] Disconnected (reason %d)\r\n", disc->reason);
            wifi_connected = false;
            if (wifi_retry_count == 0) {
                /* Spawn retry task (don't block event loop) */
                xTaskCreate(wifi_retry_task, "wifi_retry", 3072, NULL, 3, NULL);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        wifi_retry_count = 0;

        /* Start HTTP + mDNS + MQTT */
        start_http_server();
        start_mdns_service();
        start_mqtt();

        /* Green strobe override for 5 seconds */
        strobe_override = true;
        strobe_override_until = esp_timer_get_time() + (5 * 1000000LL);
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    /* Set hostname */
    esp_netif_set_hostname(sta_netif, hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

static void wifi_connect_with_creds(void) {
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password) - 1);

    if (strlen(wifi_pass) == 0) {
        /* Open network: no auth threshold, accept any */
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        wifi_config.sta.password[0] = '\0';
    } else {
        /* WPA2/WPA3: set minimum auth level */
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    /* Scan all channels, not just specific one */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    wifi_retry_count = 0;
    current_mode = 6; /* Pink while connecting */

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---- Forth Words ---- */

/* Blinky / LED */
static void w_blinky(void) {
    int mode = (int)forth_pop();
    set_mode(mode);
}

static void w_led_rgb(void) {
    int b = (int)forth_pop();
    int g = (int)forth_pop();
    int r = (int)forth_pop();
    current_mode = 0;
    vTaskDelay(pdMS_TO_TICKS(20));
    set_led((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static void w_led_off(void) {
    current_mode = 0;
    vTaskDelay(pdMS_TO_TICKS(20));
    set_led(0, 0, 0);
}

static void w_mode_get(void) {
    forth_push(current_mode);
}

static void w_modes(void) {
    uart_putchar('\r'); uart_putchar('\n');
    for (int i = 0; i < NUM_MODES; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "  %d: %s\r\n", i, mode_name(i));
        const char *p = buf;
        while (*p) uart_putchar(*p++);
    }
}

/* WiFi */
static void w_wifi_ssid(void) {
    usb_print("SSID: ");
    read_line_from_serial(wifi_ssid, sizeof(wifi_ssid), true, false);
    usb_printf("SSID set to: '%s'\r\n", wifi_ssid);
}

static void w_wifi_pass(void) {
    usb_print("Password (Enter for open): ");
    read_line_from_serial(wifi_pass, sizeof(wifi_pass), false, true);
    if (strlen(wifi_pass) == 0)
        usb_print("No password (open network)\r\n");
    else
        usb_print("Password set.\r\n");
}

static void w_wifi_connect(void) {
    if (strlen(wifi_ssid) == 0) {
        usb_print("No SSID set. Use wifi-ssid first.\r\n");
        return;
    }
    nvs_save_wifi_creds();
    usb_printf("Saved. Connecting to '%s'...\r\n", wifi_ssid);
    wifi_connect_with_creds();
}

static void w_wifi_status(void) {
    usb_printf("SSID:      '%s'\r\n", wifi_ssid);
    usb_printf("Connected: %s\r\n", wifi_connected ? "yes" : "no");
    if (wifi_connected && sta_netif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(sta_netif, &ip_info);
        usb_printf("IP:        " IPSTR "\r\n", IP2STR(&ip_info.ip));
    }
    usb_printf("Hostname:  %s.local\r\n", hostname_mdns);
    usb_printf("Mode:      %d (%s)\r\n", current_mode, mode_name(current_mode));
    usb_printf("Free heap: %lu bytes\r\n", (unsigned long)esp_get_free_heap_size());
}

static void w_wifi_clear(void) {
    nvs_clear_wifi_creds();
    usb_print("WiFi credentials cleared.\r\n");
}

/* MQTT Forth words */
static void w_mqtt_broker(void) {
    usb_printf("Current: %s\r\n", mqtt_broker_uri);
    usb_print("New broker URI (Enter to keep): ");
    char buf[128] = {0};
    read_line_from_serial(buf, sizeof(buf), true, true);
    if (strlen(buf) > 0) {
        strncpy(mqtt_broker_uri, buf, sizeof(mqtt_broker_uri) - 1);
        nvs_save_mqtt_broker();
        usb_printf("Broker set to: %s\r\n", mqtt_broker_uri);
        /* Restart MQTT if already running */
        if (mqtt_client) {
            esp_mqtt_client_stop(mqtt_client);
            esp_mqtt_client_destroy(mqtt_client);
            mqtt_client = NULL;
            mqtt_connected = false;
            if (wifi_connected) start_mqtt();
        }
    } else {
        usb_print("Unchanged.\r\n");
    }
}

static void w_mqtt_status(void) {
    usb_printf("Broker:    %s\r\n", mqtt_broker_uri);
    usb_printf("Topic:     %s\r\n", mqtt_topic[0] ? mqtt_topic : "(not set)");
    usb_printf("Connected: %s\r\n", mqtt_connected ? "yes" : "no");
}

static void w_mqtt_topic(void) {
    if (mqtt_topic[0] == '\0') derive_mqtt_topic();
    usb_printf("%s\r\n", mqtt_topic);
}

static void register_all_words(void) {
    /* LED / mode */
    forth_register_word("blinky", w_blinky);
    forth_register_word("led-rgb", w_led_rgb);
    forth_register_word("led-off", w_led_off);
    forth_register_word("mode?", w_mode_get);
    forth_register_word("modes", w_modes);

    /* WiFi */
    forth_register_word("wifi-ssid", w_wifi_ssid);
    forth_register_word("wifi-pass", w_wifi_pass);
    forth_register_word("wifi-connect", w_wifi_connect);
    forth_register_word("wifi-status", w_wifi_status);
    forth_register_word("wifi-clear", w_wifi_clear);

    /* MQTT */
    forth_register_word("mqtt-broker", w_mqtt_broker);
    forth_register_word("mqtt-status", w_mqtt_status);
    forth_register_word("mqtt-topic", w_mqtt_topic);
}

/* ---- Main ---- */

void app_main(void) {
    setup_usb_serial();
    vTaskDelay(pdMS_TO_TICKS(500));

    usb_print("\r\n\r\n");
    usb_print("============================================\r\n");
    usb_printf("  Claw OpenWR v%s\r\n", ESPIDFORTH_VERSION_STRING);
    usb_printf("  Build: %s %s\r\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    usb_print("  Claude Code LED Notifier\r\n");
    usb_print("============================================\r\n");

    /* Init hardware */
    led_init();
    btn_init();
    nvs_init_flash();

    /* Derive hostname from MAC */
    derive_hostname();
    usb_printf("Hostname: %s.local\r\n", hostname_mdns);

    /* Init WiFi subsystem */
    wifi_init_sta();

    /* Init Forth engine + register words */
    forth_init(FORTH_HEAP_SIZE);
    register_all_words();
    usb_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    /* Start LED task */
    xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);

    /* Load MQTT broker from NVS (falls back to default) */
    nvs_load_mqtt_broker();
    usb_printf("MQTT broker: %s\r\n", mqtt_broker_uri);

    /* Auto-connect from NVS if credentials exist */
    if (nvs_load_wifi_creds()) {
        usb_printf("Stored WiFi: '%s' — connecting...\r\n", wifi_ssid);
        wifi_connect_with_creds();
    } else {
        usb_print("No stored WiFi. Configure via REPL:\r\n");
        usb_print("  wifi-ssid      Set SSID\r\n");
        usb_print("  wifi-pass      Set password\r\n");
        usb_print("  wifi-connect   Connect and save\r\n");
        current_mode = 0;
    }

    usb_print("\r\nCommands: N blinky, modes, wifi-status, wifi-clear\r\n");
    usb_print("  mqtt-broker, mqtt-status, mqtt-topic\r\n");
    usb_print("Hold button 5s to clear WiFi + reboot.\r\n\r\n");

    /* Run Forth REPL (blocks forever) */
    forth_repl(uart_getchar, uart_putchar);

    forth_deinit();
}
