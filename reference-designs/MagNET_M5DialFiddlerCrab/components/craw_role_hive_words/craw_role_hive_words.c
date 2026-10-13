/*
 * craw_role_hive_words — stack-based hive-KV Forth words for role bundles.
 *
 * The per-node kv-get / kv-put REPL words are interactive — they prompt for
 * the key on the console, which would hang a role-tick forever. These are
 * the programmatic versions. Static buffers are safe: FFI words only ever
 * run under the engine lock (ESPIDFORTH >= 0.5.0), and hkv-run's nested
 * forth_eval is why that lock is recursive. The 2 s kv-get timeout bounds
 * how long a tick can block.
 *
 * Apache License 2.0, Copyright 2026 IoTone, Inc.
 */

#include "craw_role_hive_words.h"

#include <string.h>
#include <stdlib.h>

#include "forth_core.h"
#include "craw_hive.h"

/* Role-word values are short (commands, status strings) — capping the value
 * buffer well below CRAW_HIVE_KV_VALUE_MAX keeps ~3 KB of BSS off DRAM-tight
 * hosts (the classic-ESP32 camera overflowed dram0 by 2.2 KB with the full
 * buffer). Longer values are truncated safely by the kv layer; bundles that
 * need bulk data should fetch it through the C side. */
#define HKV_VAL_MAX 256
static char s_hkv_key[CRAW_HIVE_KV_KEY_MAX + 1];
static char s_hkv_val[HKV_VAL_MAX + 1];

static void pop_str(char *out, size_t cap) {
    intptr_t len  = forth_pop();
    intptr_t addr = forth_pop();
    size_t n = (addr && len > 0) ? (size_t)len : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(out, (const void *)addr, n);
    out[n] = '\0';
}

static void w_hkv_put(void) {
    pop_str(s_hkv_key, sizeof(s_hkv_key));
    pop_str(s_hkv_val, sizeof(s_hkv_val));
    craw_hive_node_kv_put(s_hkv_key, s_hkv_val);
}

static void w_hkv_get(void) {
    pop_str(s_hkv_key, sizeof(s_hkv_key));
    int rc = craw_hive_node_kv_get(s_hkv_key, s_hkv_val, sizeof(s_hkv_val), 2000);
    if (rc == 0) {
        forth_push((intptr_t)s_hkv_val);
        forth_push((intptr_t)strlen(s_hkv_val));
        forth_push(-1);
    } else {
        forth_push(0);
    }
}

static void w_hkv_run(void) {
    pop_str(s_hkv_key, sizeof(s_hkv_key));
    int rc = craw_hive_node_kv_get(s_hkv_key, s_hkv_val, sizeof(s_hkv_val), 2000);
    if (rc != 0 || !s_hkv_val[0]) return;
    /* Clear first (at-most-once), and eval a private copy — a nested hkv
     * word inside the command would clobber the static buffers. */
    char key[CRAW_HIVE_KV_KEY_MAX + 1];
    strcpy(key, s_hkv_key);
    craw_hive_node_kv_put(key, "");
    char *cmd = strdup(s_hkv_val);
    if (!cmd) return;
    forth_eval(cmd);
    free(cmd);
}

void craw_role_bundle_register_hive_words(void) {
    forth_register_word("hkv-put$", w_hkv_put);
    forth_register_word("hkv-get$", w_hkv_get);
    forth_register_word("hkv-run",  w_hkv_run);
}
