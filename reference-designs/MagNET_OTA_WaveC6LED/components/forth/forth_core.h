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

// Set I/O callbacks WITHOUT entering the blocking REPL, so forth_eval()'s
// output (., .s, ." ...) is routed. Use when an external dispatcher owns the
// input stream and drives the engine line-by-line via forth_eval().
// (Added for the MagNET dual-mode HCP/Forth link, design proposal §12.4.)
void forth_set_io(int (*get_char)(void), void (*put_char)(int));

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
/*
 * DICTIONARY SAVEPOINT — the primitive that makes hot code swap safe to attempt
 * on a device you cannot physically reach.
 *
 * The dictionary is append-only and find_word() searches BACKWARD, so a
 * redefinition shadows rather than mutates. That means the entire state is three
 * fill pointers, and rolling back is truncating them: every word defined since
 * the savepoint disappears and any word it shadowed becomes visible again.
 */
typedef struct {
    int dict_count;
    int code_ptr;
    int heap_used;
} forth_savepoint_t;

void forth_save(forth_savepoint_t *sp);
void forth_restore(const forth_savepoint_t *sp);

/*
 * Count of "? ..." errors reported since boot. forth_eval() returns 0 whether
 * or not the text made sense, so this is the only way for a caller to find out
 * that a bundle referenced an undefined word — which is precisely the failure a
 * rollback exists for.
 */
int  forth_error_count(void);

void forth_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // FORTH_CORE_H
