/**
 * craw_serial.h — Platform-abstracted serial I/O for Claw projects
 *
 * Provides a uniform serial API across:
 *   - ESP32-S3 / ESP32-C3  (USB-Serial-JTAG)
 *   - Classic ESP32         (UART console via stdio)
 *
 * All functions are safe to call from any FreeRTOS task.
 */
#ifndef CRAW_SERIAL_H
#define CRAW_SERIAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the serial back-end.
 * Must be called once before any other craw_serial function.
 */
void craw_serial_init(void);

/**
 * Read a single character (blocking up to ~10 ms).
 * Returns the character value (0-255) or -1 if nothing available.
 */
int craw_serial_getchar(void);

/**
 * Write a single character (blocking up to ~100 ms).
 */
void craw_serial_putchar(int c);

/**
 * Write a null-terminated string.
 */
void craw_serial_print(const char *s);

/**
 * Formatted print (printf-style).  Output is truncated at 256 bytes.
 */
void craw_serial_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/**
 * Interactive line-input with drain, first-enter-skip, backspace,
 * optional echo, and optional allow-empty semantics.
 *
 * @param buf         Destination buffer.
 * @param maxlen      Size of buf (including NUL terminator).
 * @param echo        If true characters are echoed; otherwise '*' is echoed.
 * @param allow_empty If false the user must type at least one character.
 */
void craw_serial_read_line(char *buf, int maxlen, bool echo, bool allow_empty);

#ifdef __cplusplus
}
#endif

#endif /* CRAW_SERIAL_H */
