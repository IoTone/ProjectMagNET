/*
 * M5Stamp3CU Blinky E4TH
 *
 * NeoPixel LED patterns on M5Stamp C3U, scripted in Forth.
 * Button (GPIO 9, active low) cycles through modes on release.
 * LED (SK6812/WS2812 on GPIO 2) driven via ESP-IDF led_strip RMT driver.
 *
 * Modes (button cycles, or type "blinky N" at REPL):
 *   0: Off
 *   1: Random color flash
 *   2: Slow breathing of a random color
 *   3: Fast strobe of random colors
 *   4: Rainbow cycle
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/temperature_sensor.h"
#include "led_strip.h"
#include "forth_core.h"
#include "forth_version.h"

/* Phase 4A — hive provisioning (R3, R4) */
#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"

#define LED_GPIO        2
#define BTN_GPIO        9
#define FORTH_HEAP_SIZE (64 * 1024)
#define NUM_MODES       5

static led_strip_handle_t led_strip = NULL;
static volatile int btn_released = 0;

/* ESP32-C3 internal die-temperature sensor. ±5–10 °C absolute, but stable
 * for relative readings — fine as a "device load" indicator, not a
 * thermostat. Drop in a DS18B20 / SHT4x for ambient. */
static temperature_sensor_handle_t s_temp_sensor = NULL;
static volatile int current_mode = 0;

/* Phase 4A provisioning state surfaced to Forth / LED */
static volatile int s_prov_state = 0;   /* mirrors craw_ble_prov_state_t */
static char s_ip_str[20] = "N/A";

/* ---- USB serial I/O ---- */

static void usb_print(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}

static void usb_printf(const char *fmt, ...) {
    char buf[160];
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

/* ---- Button ISR on release ---- */

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

/* ---- Mode names ---- */

static const char *mode_name(int mode) {
    switch (mode) {
        case 0: return "Off";
        case 1: return "Random Flash";
        case 2: return "Slow Breathe";
        case 3: return "Fast Strobe";
        case 4: return "Rainbow";
        default: return "Unknown";
    }
}

static void set_mode(int mode) {
    if (mode < 0 || mode >= NUM_MODES) mode = 0;
    current_mode = mode;
    usb_printf("\r\nMode %d: %s\r\n", current_mode, mode_name(current_mode));
}

static bool check_button(void) {
    if (btn_released) {
        btn_released = 0;
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(BTN_GPIO) == 1) {
            set_mode((current_mode + 1) % NUM_MODES);
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

/* Mode 0: Off */
static void pattern_off(void) {
    set_led(0, 0, 0);
    while (current_mode == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        check_button();
    }
}

/* Mode 1: Random color flash — new random color every 200ms */
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

/* Mode 2: Slow breathing — fade a random hue in/out */
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

/* Mode 3: Fast strobe — random colors, 50ms on / 50ms off */
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

/* Mode 4: Rainbow — cycle through full hue spectrum every ~2 seconds */
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

/* ---- LED task ---- */

static void led_task(void *arg) {
    while (1) {
        switch (current_mode) {
            case 0: pattern_off(); break;
            case 1: pattern_random_flash(); break;
            case 2: pattern_breathe(); break;
            case 3: pattern_strobe(); break;
            case 4: pattern_rainbow(); break;
        }
    }
}

/* ---- Phase 4A: WiFi + BLE provisioning (R3, R4) ---- */

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
        case CRAW_WIFI_EVENT_CONNECTED:
            craw_wifi_get_ip_str(s_ip_str, sizeof(s_ip_str));
            usb_printf("\r\n[WiFi] connected, IP: %s\r\n", s_ip_str);
            craw_ble_provision_set_ip(s_ip_str);
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTED);
            s_prov_state = CRAW_BLE_PROV_CONNECTED;
            break;
        case CRAW_WIFI_EVENT_DISCONNECTED:
            usb_print("\r\n[WiFi] disconnected\r\n");
            strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
            craw_ble_provision_set_ip(s_ip_str);
            break;
        case CRAW_WIFI_EVENT_CONNECT_FAILED:
            usb_print("\r\n[WiFi] connect failed\r\n");
            strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
            craw_ble_provision_set_ip(s_ip_str);
            craw_ble_provision_set_status(CRAW_BLE_PROV_FAILED);
            s_prov_state = CRAW_BLE_PROV_FAILED;
            break;
    }
}

static void on_prov_event(craw_ble_prov_state_t state,
                          const char *ssid, const char *pass, void *ctx) {
    (void)ctx;
    s_prov_state = (int)state;
    switch (state) {
        case CRAW_BLE_PROV_CREDS_RECEIVED:
            usb_printf("\r\n[PROV] creds received: ssid='%s' pass=%d chars\r\n",
                       ssid, (int)strlen(pass ? pass : ""));
            break;
        case CRAW_BLE_PROV_COMMIT_REQUESTED:
            if (!ssid || !ssid[0]) {
                usb_print("\r\n[PROV] commit with empty SSID, ignoring\r\n");
                break;
            }
            usb_printf("\r\n[PROV] commit -> connecting to '%s'\r\n", ssid);
            craw_nvs_save_wifi_creds(ssid, pass ? pass : "");
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
            s_prov_state = CRAW_BLE_PROV_CONNECTING;
            craw_wifi_connect(ssid, pass ? pass : "");
            break;
        default:
            break;
    }
}

/* ---- Forth FFI Words ---- */

/* ( -- ) Report provisioning + WiFi state */
static void w_prov_status(void) {
    const char *labels[] = {
        "IDLE", "CREDS_RECEIVED", "COMMIT_REQUESTED",
        "CONNECTING", "CONNECTED", "FAILED",
    };
    int st = s_prov_state;
    if (st < 0 || st >= (int)(sizeof(labels)/sizeof(labels[0]))) st = 0;
    char ssid[33], pass[65];
    bool has_creds = craw_nvs_load_wifi_creds(ssid, pass);
    usb_printf("\r\nprov:  %s\r\n", labels[st]);
    usb_printf("ble:   %s\r\n", craw_ble_provision_device_name());
    usb_printf("wifi:  %s\r\n", craw_wifi_is_connected() ? "connected" : "down");
    usb_printf("ssid:  %s\r\n", has_creds ? ssid : "(none)");
    usb_printf("ip:    %s\r\n", s_ip_str);
}

/* ( -- ) Clear WiFi creds and re-advertise as unprovisioned */
static void w_prov_reset(void) {
    char ssid[33], pass[65];
    craw_nvs_clear_wifi_creds(ssid, pass);
    craw_wifi_disconnect();
    strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
    craw_ble_provision_set_ip(s_ip_str);
    craw_ble_provision_set_status(CRAW_BLE_PROV_IDLE);
    s_prov_state = CRAW_BLE_PROV_IDLE;
    craw_ble_provision_advertise();
    usb_print("\r\nProvisioning reset. Advertising.\r\n");
}

/* ( mode -- ) Set the blinky mode: 0=off 1=flash 2=breathe 3=strobe 4=rainbow */
static void w_blinky(void) {
    int mode = (int)forth_pop();
    set_mode(mode);
}

/* ( r g b -- ) Set the LED to a specific color (stops current pattern, mode 0) */
static void w_led_rgb(void) {
    int b = (int)forth_pop();
    int g = (int)forth_pop();
    int r = (int)forth_pop();
    current_mode = 0;  /* stop pattern task */
    vTaskDelay(pdMS_TO_TICKS(20));  /* let pattern task notice */
    set_led((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* ( -- ) Turn off the LED */
static void w_led_off(void) {
    current_mode = 0;
    vTaskDelay(pdMS_TO_TICKS(20));
    set_led(0, 0, 0);
}

/* ( -- mode ) Push current mode onto stack */
static void w_mode_get(void) {
    forth_push(current_mode);
}

/* ( -- ) Print available modes */
static void w_modes(void) {
    uart_putchar('\r');
    uart_putchar('\n');
    for (int i = 0; i < NUM_MODES; i++) {
        char buf[40];
        snprintf(buf, sizeof(buf), "  %d: %s\r\n", i, mode_name(i));
        const char *p = buf;
        while (*p) uart_putchar(*p++);
    }
}

/* ---- ESP32-C3 internal temperature sensor ----
 * Die temp, not ambient. ±5–10 °C absolute accuracy but stable for
 * relative work. Useful as a "device load" indicator and as a placeholder
 * data source until a real ambient sensor (DS18B20 / SHT4x) is wired up. */

static void temp_sensor_init(void) {
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_temp_sensor) != ESP_OK) {
        usb_print("[temp] sensor install failed\r\n");
        return;
    }
    if (temperature_sensor_enable(s_temp_sensor) != ESP_OK) {
        usb_print("[temp] sensor enable failed\r\n");
        s_temp_sensor = NULL;
        return;
    }
}

/* Returns die temp in centi-Celsius (e.g. 4530 = 45.30 °C).
 * INT32_MIN on error so callers can distinguish "sensor unavailable"
 * from a valid reading near 0 °C. */
static int32_t read_centi_celsius(void) {
    if (!s_temp_sensor) return INT32_MIN;
    float c;
    if (temperature_sensor_get_celsius(s_temp_sensor, &c) != ESP_OK) return INT32_MIN;
    return (int32_t)(c * 100.0f + (c >= 0 ? 0.5f : -0.5f));
}

/* ( -- ) Print current die temperature to the REPL. */
static void w_cpu_temp(void) {
    int32_t cc = read_centi_celsius();
    if (cc == INT32_MIN) { usb_print("\r\n[temp] sensor unavailable\r\n"); return; }
    usb_printf("\r\nCPU temp: %ld.%02ld C\r\n",
               (long)(cc / 100), (long)((cc < 0 ? -cc : cc) % 100));
}

/* ( -- centi-c ) Push die temp as centi-Celsius onto the Forth stack.
 * Forth stack cells are intptr_t (no float in this build); centi-C gives
 * 0.01 °C resolution and lets users compose: cpu-temp? 5000 > IF ." HOT" THEN. */
static void w_cpu_temp_q(void) {
    forth_push(read_centi_celsius());
}

static void register_blinky_words(void) {
    forth_register_word("blinky", w_blinky);
    forth_register_word("led-rgb", w_led_rgb);
    forth_register_word("led-off", w_led_off);
    forth_register_word("mode?", w_mode_get);
    forth_register_word("modes", w_modes);
    forth_register_word("prov-status", w_prov_status);
    forth_register_word("prov-reset", w_prov_reset);
    forth_register_word("cpu-temp",   w_cpu_temp);
    forth_register_word("cpu-temp?",  w_cpu_temp_q);
}

/* ---- Main ---- */

void app_main(void) {
    setup_usb_serial();
    vTaskDelay(pdMS_TO_TICKS(500));

    usb_print("\r\n\r\n");
    usb_print("============================================\r\n");
    usb_printf("  M5Stamp C3U Blinky E4TH v%s\r\n", ESPIDFORTH_VERSION_STRING);
    usb_printf("  Build: %s %s\r\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    usb_print("  NeoPixel patterns via ESPIDFORTH\r\n");
    usb_print("============================================\r\n");

    /* Initialize hardware */
    led_init();
    btn_init();
    temp_sensor_init();
    usb_print("LED on GPIO 2, Button on GPIO 9\r\n");
    usb_print("CPU temperature sensor ready\r\n");

    /* NVS + WiFi + BLE provisioning (Phase 4A) */
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_wifi_init("MagNET-biologic", on_wifi_event, NULL);
    craw_ble_provision_config_t prov_cfg = {
        .name_prefix = "MagNET-biologic",
        .role        = "spawn",
    };
    craw_ble_provision_init(&prov_cfg, on_prov_event, NULL);
    usb_printf("BLE advertising as '%s'\r\n", craw_ble_provision_device_name());

    /* Initialize Forth engine + register custom words */
    forth_init(FORTH_HEAP_SIZE);
    register_blinky_words();
    usb_print("Forth engine initialized.\r\n");
    usb_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    /* Start LED pattern task */
    xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);

    /* Auto-connect if we have stored creds */
    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
        usb_printf("Stored WiFi '%s' — auto-connecting...\r\n", ssid);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        s_prov_state = CRAW_BLE_PROV_CONNECTING;
        craw_wifi_connect(ssid, pass);
    } else {
        usb_print("No stored WiFi. Connect via BLE to provision.\r\n");
    }

    usb_print("Button cycles modes. Forth commands:\r\n");
    usb_print("  N blinky    -- set mode (0-4)\r\n");
    usb_print("  modes       -- list all modes\r\n");
    usb_print("  R G B led-rgb -- set LED color (stops pattern)\r\n");
    usb_print("  led-off     -- turn off LED\r\n");
    usb_print("  mode?       -- show current mode\r\n");
    usb_print("  prov-status -- BLE/WiFi provisioning state\r\n");
    usb_print("  cpu-temp    -- print CPU die temperature\r\n");
    usb_print("  cpu-temp?   -- push die temp in centi-Celsius\r\n");
    usb_print("  prov-reset  -- clear WiFi creds, re-advertise\r\n\r\n");

    /* Run Forth REPL (blocks forever) */
    forth_repl(uart_getchar, uart_putchar);

    forth_deinit();
}
