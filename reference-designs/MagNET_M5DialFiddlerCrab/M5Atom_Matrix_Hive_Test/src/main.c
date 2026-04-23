/*
 * M5Atom_Matrix_Hive_Test — first cross-chip peer in the MagNET hive.
 *
 * Target: M5Atom Matrix (ESP32-PICO-D4). Validates Milestone-A (BLE
 * provisioning via craw_ble_provision) and Milestone-B (hive join via
 * craw_hive_node) on classic ESP32 — the C3U project has already shown
 * both working on RISC-V. Uses the onboard 5x5 WS2812B matrix as a
 * glanceable status panel.
 *
 * Hardware:
 *   - ESP32-PICO-D4, 4 MB flash, 520 KB SRAM
 *   - 25 × WS2812B on GPIO 27 (5x5, row-major index 0..24)
 *   - Button on GPIO 39 (input-only, external pull-up on board)
 *   - UART0 @ 115200 via on-board CP2104 (no USB-serial-JTAG)
 *
 * Status layout on the 5×5 matrix (row 0 is top):
 *   Row 0: [BLE] [WiFi] [Hive] [spare] [heartbeat flash]
 *     col 0 BLE:  cyan = advertising, white = client connected
 *     col 1 WiFi: amber = connecting, green = connected, red = failed
 *     col 2 Hive: blue = discover, amber = connecting, green = joined, red = failed
 *     col 3 spare (future: role indicator)
 *     col 4 heartbeat: brief white flash on PING
 *   Rows 1..4 currently blank; reserved for future caps/role viz.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "led_strip.h"

#include "forth_core.h"
#include "forth_version.h"

#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"
#include "craw_hive.h"

static const char *TAG = "m5atom_hive";

#define LED_GPIO        27
#define LED_COUNT       25
#define BTN_GPIO        39
#define CONSOLE_UART    UART_NUM_0
#define CONSOLE_BAUD    115200
#define FORTH_HEAP_SIZE (32 * 1024)

/* ---- LED matrix ---- */
static led_strip_handle_t s_matrix = NULL;

/* Row-major (row, col) → strip index. Confirmed via M5Atom Matrix
 * data sheet: LED 0 is top-left, wiring runs left→right, row by row. */
static inline int idx_rc(int r, int c) { return r * 5 + c; }

static void matrix_clear(void) {
    if (!s_matrix) return;
    led_strip_clear(s_matrix);
}

static void matrix_set_px(int r, int c, uint8_t rr, uint8_t gg, uint8_t bb) {
    if (!s_matrix || r < 0 || r > 4 || c < 0 || c > 4) return;
    led_strip_set_pixel(s_matrix, idx_rc(r, c), rr, gg, bb);
}

static void matrix_refresh(void) {
    if (!s_matrix) return;
    led_strip_refresh(s_matrix);
}

static void matrix_init(void) {
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = LED_COUNT,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt_cfg, &s_matrix));
    matrix_clear();
    matrix_refresh();
}

/* ---- Status model ---- */
typedef enum { BLE_OFF, BLE_ADVERT, BLE_CONNECTED } ble_ui_t;
typedef enum { WIFI_OFF, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED } wifi_ui_t;

static volatile ble_ui_t  s_ble_ui  = BLE_OFF;
static volatile wifi_ui_t s_wifi_ui = WIFI_OFF;
static volatile craw_hive_node_state_t s_hive_ui = CRAW_HIVE_NODE_OFFLINE;
static volatile int64_t   s_heartbeat_until_ms = 0;
static char s_ip_str[20] = "N/A";

static void paint_status(void) {
    if (!s_matrix) return;
    matrix_clear();

    switch (s_ble_ui) {
        case BLE_ADVERT:    matrix_set_px(0, 0, 0, 80, 80); break;   /* cyan */
        case BLE_CONNECTED: matrix_set_px(0, 0, 80, 80, 80); break;  /* white */
        default: break;
    }
    switch (s_wifi_ui) {
        case WIFI_CONNECTING: matrix_set_px(0, 1, 80, 50, 0); break; /* amber */
        case WIFI_CONNECTED:  matrix_set_px(0, 1, 0, 80, 0); break;  /* green */
        case WIFI_FAILED:     matrix_set_px(0, 1, 80, 0, 0); break;  /* red   */
        default: break;
    }
    switch (s_hive_ui) {
        case CRAW_HIVE_NODE_DISCOVER:   matrix_set_px(0, 2, 0, 0, 80); break;  /* blue  */
        case CRAW_HIVE_NODE_CONNECTING: matrix_set_px(0, 2, 80, 50, 0); break; /* amber */
        case CRAW_HIVE_NODE_JOINED:     matrix_set_px(0, 2, 0, 80, 0); break;  /* green */
        case CRAW_HIVE_NODE_BACKOFF:    matrix_set_px(0, 2, 80, 0, 0); break;  /* red   */
        default: break;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < s_heartbeat_until_ms) {
        matrix_set_px(0, 4, 80, 80, 80); /* heartbeat flash */
    }

    matrix_refresh();
}

static void ui_task(void *arg) {
    (void)arg;
    while (1) {
        paint_status();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---- UART console ---- */
static void console_init(void) {
    uart_config_t cfg = {
        .baud_rate  = CONSOLE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(CONSOLE_UART, 512, 512, 0, NULL, 0);
    uart_param_config(CONSOLE_UART, &cfg);
}

static void uart_print(const char *s) {
    uart_write_bytes(CONSOLE_UART, s, strlen(s));
}

static void uart_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uart_print(buf);
}

static int console_getchar(void) {
    uint8_t c;
    int n = uart_read_bytes(CONSOLE_UART, &c, 1, pdMS_TO_TICKS(10));
    if (n <= 0) return -1;
    return c;
}

static void console_putchar(int c) {
    uint8_t ch = (uint8_t)c;
    uart_write_bytes(CONSOLE_UART, &ch, 1);
}

/* ---- Button ---- */
static volatile int s_btn_released = 0;
static void IRAM_ATTR btn_isr(void *arg) {
    if (gpio_get_level(BTN_GPIO) == 1) s_btn_released = 1;
}

static void btn_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE, /* GPIO 39 is input-only; board has pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_GPIO, btn_isr, NULL);
}

/* ---- WiFi + BLE provisioning callbacks ---- */

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
        case CRAW_WIFI_EVENT_CONNECTED:
            craw_wifi_get_ip_str(s_ip_str, sizeof(s_ip_str));
            s_wifi_ui = WIFI_CONNECTED;
            uart_printf("\r\n[WiFi] connected, IP: %s\r\n", s_ip_str);
            craw_ble_provision_set_ip(s_ip_str);
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTED);
            break;
        case CRAW_WIFI_EVENT_DISCONNECTED:
            s_wifi_ui = WIFI_OFF;
            uart_print("\r\n[WiFi] disconnected\r\n");
            strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
            craw_ble_provision_set_ip(s_ip_str);
            break;
        case CRAW_WIFI_EVENT_CONNECT_FAILED:
            s_wifi_ui = WIFI_FAILED;
            uart_print("\r\n[WiFi] failed\r\n");
            strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
            craw_ble_provision_set_ip(s_ip_str);
            craw_ble_provision_set_status(CRAW_BLE_PROV_FAILED);
            break;
    }
}

static void on_prov_event(craw_ble_prov_state_t state,
                          const char *ssid, const char *pass, void *ctx) {
    (void)ctx;
    switch (state) {
        case CRAW_BLE_PROV_CREDS_RECEIVED:
            s_ble_ui = BLE_CONNECTED;
            uart_printf("\r\n[PROV] creds: ssid='%s'\r\n", ssid);
            break;
        case CRAW_BLE_PROV_COMMIT_REQUESTED:
            if (!ssid || !ssid[0]) break;
            uart_printf("\r\n[PROV] commit -> '%s'\r\n", ssid);
            craw_nvs_save_wifi_creds(ssid, pass ? pass : "");
            s_wifi_ui = WIFI_CONNECTING;
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
            craw_wifi_connect(ssid, pass ? pass : "");
            break;
        default:
            break;
    }
}

/* ---- Hive node callback ---- */
static void on_hive_state(craw_hive_node_state_t state,
                          const char *info, void *ctx) {
    (void)ctx;
    s_hive_ui = state;
    uart_printf("\r\n[HIVE] state=%d (%s)\r\n", (int)state, info ? info : "");
    /* Flash heartbeat LED briefly on any state transition. */
    s_heartbeat_until_ms = (esp_timer_get_time() / 1000) + 200;
}

/* ---- Hive start (deferred until WiFi is up) ---- */
static bool s_hive_started = false;
static const char *s_caps[] = { "led", "matrix", "button", NULL };
static char s_node_id[40] = {0};

static void derive_node_id(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node_id, sizeof(s_node_id), "MagNET-biologic-%02x%02x", mac[4], mac[5]);
}

static void maybe_start_hive(void) {
    if (s_hive_started) return;
    if (!craw_wifi_is_connected()) return;
    derive_node_id();
    static craw_hive_node_config_t ncfg;
    ncfg = (craw_hive_node_config_t){
        .node_id        = s_node_id,
        .hive_id        = "beehive-1",
        .role_requested = "spawn",
        .caps           = s_caps,
        .chip           = "ESP32",
        .fw             = "0.1.0",
        .secret         = (const uint8_t *)CRAW_HIVE_DEV_SECRET,
        .on_state       = on_hive_state,
        .on_state_ctx   = NULL,
    };
    if (craw_hive_node_start(&ncfg) == 0) {
        s_hive_started = true;
        uart_print("[HIVE] node started\r\n");
    }
}

/* Poll WiFi + button from a low-priority task. */
static void housekeeping_task(void *arg) {
    (void)arg;
    while (1) {
        if (s_btn_released) {
            s_btn_released = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BTN_GPIO) == 1) {
                /* Short press: heartbeat flash for user-visible "alive" check. */
                s_heartbeat_until_ms = (esp_timer_get_time() / 1000) + 500;
            }
        }
        maybe_start_hive();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ---- Forth words ---- */

static void w_prov_status(void) {
    char ssid[33], pass[65];
    bool has = craw_nvs_load_wifi_creds(ssid, pass);
    uart_printf("\r\nble:   %s\r\n", craw_ble_provision_device_name());
    uart_printf("wifi:  %s\r\n", craw_wifi_is_connected() ? "connected" : "down");
    uart_printf("ssid:  %s\r\n", has ? ssid : "(none)");
    uart_printf("ip:    %s\r\n", s_ip_str);
}

static void w_prov_reset(void) {
    char ssid[33], pass[65];
    craw_nvs_clear_wifi_creds(ssid, pass);
    craw_wifi_disconnect();
    craw_ble_provision_set_status(CRAW_BLE_PROV_IDLE);
    craw_ble_provision_advertise();
    s_wifi_ui = WIFI_OFF;
    uart_print("\r\nReset. Advertising again.\r\n");
}

static void w_hive_status(void) {
    const char *labels[] = { "OFFLINE","DISCOVER","CONNECTING","JOINED","BACKOFF" };
    int st = (int)craw_hive_node_state();
    if (st < 0 || st > 4) st = 0;
    const char *sid = craw_hive_node_session_id();
    uart_printf("\r\nhive:    %s\r\n", labels[st]);
    uart_printf("node:    %s\r\n", s_node_id[0] ? s_node_id : "(unset)");
    uart_printf("session: %s\r\n", sid ? sid : "(none)");
}

/* ( -- ) Test pattern: light every pixel briefly, then restore status. */
static void w_matrix_test(void) {
    if (!s_matrix) return;
    for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel(s_matrix, i, 30, 30, 30);
    }
    matrix_refresh();
    vTaskDelay(pdMS_TO_TICKS(400));
    paint_status();
}

/* ( r c rr gg bb -- ) Set a specific pixel; next status repaint overwrites. */
static void w_matrix_pixel(void) {
    int bb = (int)forth_pop();
    int gg = (int)forth_pop();
    int rr = (int)forth_pop();
    int c  = (int)forth_pop();
    int r  = (int)forth_pop();
    matrix_set_px(r, c, (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
    matrix_refresh();
}

static void register_forth_words(void) {
    forth_register_word("prov-status",  w_prov_status);
    forth_register_word("prov-reset",   w_prov_reset);
    forth_register_word("hive-status",  w_hive_status);
    forth_register_word("matrix-test",  w_matrix_test);
    forth_register_word("matrix-pixel", w_matrix_pixel);
}

/* ---- app_main ---- */
void app_main(void) {
    console_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    uart_print("\r\n\r\n===================================\r\n");
    uart_printf(  "  M5Atom Matrix Hive Test v0.1.0\r\n");
    uart_printf(  "  ESPIDFORTH %s\r\n", ESPIDFORTH_VERSION_STRING);
    uart_print(   "  ESP32 / M5Atom Matrix (5x5 WS2812B)\r\n");
    uart_print(   "===================================\r\n");

    matrix_init();
    btn_init();
    uart_printf("LED strip on GPIO %d (%d px), button on GPIO %d\r\n",
                LED_GPIO, LED_COUNT, BTN_GPIO);

    /* NVS + WiFi + BLE provisioning */
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_wifi_init("MagNET-biologic", on_wifi_event, NULL);

    craw_ble_provision_config_t pcfg = {
        .name_prefix = "MagNET-biologic",
        .role        = "spawn",
    };
    craw_ble_provision_init(&pcfg, on_prov_event, NULL);
    s_ble_ui = BLE_ADVERT;
    uart_printf("BLE: %s\r\n", craw_ble_provision_device_name());

    /* Forth */
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    uart_printf("Forth ready. Free heap: %lu bytes\r\n",
                (unsigned long)esp_get_free_heap_size());

    /* Background tasks */
    xTaskCreate(ui_task,           "ui",       3072, NULL, 2, NULL);
    xTaskCreate(housekeeping_task, "keep",     4096, NULL, 3, NULL);

    /* Auto-connect if we already have creds */
    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
        uart_printf("Stored WiFi '%s' — auto-connect\r\n", ssid);
        s_wifi_ui = WIFI_CONNECTING;
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        craw_wifi_connect(ssid, pass);
    } else {
        uart_print("No stored WiFi — provision via BLE.\r\n");
    }

    uart_print("\r\nForth commands:\r\n");
    uart_print("  prov-status  -- BLE / WiFi state\r\n");
    uart_print("  prov-reset   -- clear creds, re-advertise\r\n");
    uart_print("  hive-status  -- hive node state + session id\r\n");
    uart_print("  matrix-test  -- flash every pixel once\r\n");
    uart_print("  R C Rr Gg Bb matrix-pixel  -- set one pixel\r\n\r\n");

    forth_repl(console_getchar, console_putchar);
    forth_deinit();
}
