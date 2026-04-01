/*
 * forth_core.h - Public API for the Forth engine (ESP-IDF port)
 *
 * This provides a C-callable interface to the Forth interpreter.
 * The underlying implementation is based on ESP32forth v7.0.8.0
 * (https://esp32forth.appspot.com/) adapted for ESP-IDF.
 *
 * Licensed under the Apache License, Version 2.0
 */

#ifndef FORTH_CORE_H
#define FORTH_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the Forth engine with given dictionary heap size
int forth_init(int heap_size_bytes);

// Run the Forth REPL (blocking, reads from provided getchar/putchar)
void forth_repl(int (*get_char)(void), void (*put_char)(int));

// Execute a Forth string
int forth_eval(const char *text);

// Get memory usage info
int forth_heap_used(void);
int forth_heap_free(void);

// Cleanup
void forth_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // FORTH_CORE_H
