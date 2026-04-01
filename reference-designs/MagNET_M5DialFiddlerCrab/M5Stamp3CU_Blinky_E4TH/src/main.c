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
#include "led_strip.h"
#include "forth_core.h"
#include "forth_version.h"

#define LED_GPIO        2
#define BTN_GPIO        9
#define FORTH_HEAP_SIZE (64 * 1024)
#define NUM_MODES       5

static led_strip_handle_t led_strip = NULL;
static volatile int btn_released = 0;
static volatile int current_mode = 0;

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

/* ---- Forth FFI Words ---- */

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

static void register_blinky_words(void) {
    forth_register_word("blinky", w_blinky);
    forth_register_word("led-rgb", w_led_rgb);
    forth_register_word("led-off", w_led_off);
    forth_register_word("mode?", w_mode_get);
    forth_register_word("modes", w_modes);
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
    usb_print("LED on GPIO 2, Button on GPIO 9\r\n");

    /* Initialize Forth engine + register custom words */
    forth_init(FORTH_HEAP_SIZE);
    register_blinky_words();
    usb_print("Forth engine initialized.\r\n");
    usb_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    /* Start LED pattern task */
    xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);

    usb_print("Button cycles modes. Forth commands:\r\n");
    usb_print("  N blinky    -- set mode (0-4)\r\n");
    usb_print("  modes       -- list all modes\r\n");
    usb_print("  R G B led-rgb -- set LED color (stops pattern)\r\n");
    usb_print("  led-off     -- turn off LED\r\n");
    usb_print("  mode?       -- show current mode\r\n\r\n");

    /* Run Forth REPL (blocks forever) */
    forth_repl(uart_getchar, uart_putchar);

    forth_deinit();
}
