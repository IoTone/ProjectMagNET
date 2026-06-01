/**
 * craw_serial.c — Platform-abstracted serial I/O
 *
 * Back-end selection (compile-time):
 *   ESP32-S3 / ESP32-C3  →  USB-Serial-JTAG driver
 *   Classic ESP32 / other →  UART driver (raw, unbuffered)
 */

#include "craw_serial.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------ */
/* Back-end: USB-Serial-JTAG (ESP32-S3, ESP32-C3)                     */
/* ------------------------------------------------------------------ */
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)

#include "driver/usb_serial_jtag.h"

void craw_serial_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&cfg);
}

int craw_serial_getchar(void)
{
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    if (n <= 0) return -1;
    return (int)c;
}

void craw_serial_putchar(int c)
{
    uint8_t ch = (uint8_t)c;
    usb_serial_jtag_write_bytes(&ch, 1, pdMS_TO_TICKS(100));
}

void craw_serial_print(const char *s)
{
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s),
                                pdMS_TO_TICKS(500));
}

/* ------------------------------------------------------------------ */
/* Back-end: UART driver (classic ESP32, ESP32-C6, etc.)               */
/* ------------------------------------------------------------------ */
#else

#include "driver/uart.h"

#define CRAW_UART_NUM    UART_NUM_0
#define CRAW_UART_BUF    256

static bool uart_installed = false;

void craw_serial_init(void)
{
    if (uart_installed) return;

    /* Install UART driver on UART0 (default console).
     * We use the driver for raw byte-at-a-time RX. */
    uart_config_t uart_config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(CRAW_UART_NUM, &uart_config);
    /* Don't set pins — UART0 uses default TX=1, RX=3 */
    uart_driver_install(CRAW_UART_NUM, CRAW_UART_BUF, 0, 0, NULL, 0);
    uart_installed = true;
}

int craw_serial_getchar(void)
{
    uint8_t c;
    int n = uart_read_bytes(CRAW_UART_NUM, &c, 1, pdMS_TO_TICKS(10));
    if (n <= 0) return -1;
    return (int)c;
}

void craw_serial_putchar(int c)
{
    uint8_t ch = (uint8_t)c;
    uart_write_bytes(CRAW_UART_NUM, (const char *)&ch, 1);
}

void craw_serial_print(const char *s)
{
    uart_write_bytes(CRAW_UART_NUM, s, strlen(s));
}

#endif /* back-end selection */

/* ------------------------------------------------------------------ */
/* Common (platform-independent)                                       */
/* ------------------------------------------------------------------ */

void craw_serial_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    craw_serial_print(buf);
}

void craw_serial_read_line(char *buf, int maxlen, bool echo, bool allow_empty)
{
    /* Short delay then drain any stale bytes in the receive buffer. */
    vTaskDelay(pdMS_TO_TICKS(50));
    while (1) {
        int drain = craw_serial_getchar();
        if (drain < 0) break;
    }

    bool first_enter_skipped = false;
    int  pos = 0;

    while (pos < maxlen - 1) {
        int ch = craw_serial_getchar();
        if (ch < 0) continue;

        /* CR / LF handling */
        if (ch == '\r' || ch == '\n') {
            if (!first_enter_skipped) {
                first_enter_skipped = true;
                continue;
            }
            if (pos == 0 && !allow_empty) continue;
            break;
        }

        first_enter_skipped = true;

        /* Backspace / DEL */
        if (ch == 8 || ch == 127) {
            if (pos > 0) {
                pos--;
                if (echo) craw_serial_print("\b \b");
            }
            continue;
        }

        /* Printable character */
        if (ch >= 32) {
            buf[pos++] = (char)ch;
            if (echo)
                craw_serial_putchar(ch);
            else
                craw_serial_putchar('*');
        }
    }

    buf[pos] = '\0';
    craw_serial_print("\r\n");
}
