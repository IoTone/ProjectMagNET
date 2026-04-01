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

#include <stdint.h>

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

// Register an external C function as a Forth primitive word.
// Call after forth_init(), before forth_repl().
// fn receives no args — use forth_push/forth_pop for stack access.
typedef void (*forth_word_fn)(void);
int forth_register_word(const char *name, forth_word_fn fn);

// Stack access for external FFI words
void forth_push(intptr_t value);
intptr_t forth_pop(void);

// Cleanup
void forth_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // FORTH_CORE_H
