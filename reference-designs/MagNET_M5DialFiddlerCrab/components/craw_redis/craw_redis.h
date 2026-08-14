#ifndef CRAW_REDIS_H
#define CRAW_REDIS_H

/* craw_redis — RESP2-compatible TCP server for the MagNET Capsule Scribe.
 *
 * Storage is pluggable so the same server can sit on top of NVS (Capsule)
 * or FAT-on-SD (future ESP32Cam variant). v1 ships only the NVS backend.
 *
 * Security model — see plan in conversation history:
 *   - bind defaults to 0.0.0.0; warning logged on every start
 *   - no AUTH in v1 (parsed but no-op so redis-cli -a doesn't error)
 *   - configuration profiles persisted in NVS
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* User-key length. Keys live directly in NVS (15-char limit) prefixed with
 * 's' or 'l' so the cap is 14 chars. Documented limit; v2 SD backend lifts. */
#define CRAW_REDIS_KEY_MAX        14

/* Per-string-value limit. NVS blob fragmentation gets nasty above ~1.5 KB,
 * so we cap conservatively. */
#define CRAW_REDIS_VALUE_MAX      1024

/* List storage caps — entire list lives in one NVS blob. */
#define CRAW_REDIS_LIST_TOTAL_MAX 4096    /* total bytes per list */
#define CRAW_REDIS_LIST_ENTRIES_MAX 64
#define CRAW_REDIS_LIST_ENTRY_MAX 1024    /* per-entry */

/* Wire limits. */
#define CRAW_REDIS_INLINE_MAX     2048
#define CRAW_REDIS_BULK_MAX       16384
#define CRAW_REDIS_ARGS_MAX       16
#define CRAW_REDIS_REPLY_MAX      8192

#define CRAW_REDIS_DEFAULT_PORT   6379
#define CRAW_REDIS_MAX_CLIENTS    4

typedef enum {
    CRAW_REDIS_PROFILE_LOCAL  = 0,    /* 127.0.0.1 : 6379  */
    CRAW_REDIS_PROFILE_LAN    = 1,    /* 0.0.0.0 : 6379    */
    CRAW_REDIS_PROFILE_QUIET  = 2,    /* 0.0.0.0 : 16379   */
    CRAW_REDIS_PROFILE_CUSTOM = 3,
} craw_redis_profile_t;

typedef struct {
    char     bind[16];           /* "0.0.0.0" / "127.0.0.1" / custom */
    uint16_t port;
} craw_redis_config_t;

typedef struct {
    bool     running;
    char     bind[16];
    uint16_t port;
    int      clients;
    uint64_t commands_total;
    uint64_t bytes_in;
    uint64_t bytes_out;
} craw_redis_stats_t;

/* ---- Storage interface (pluggable) ----
 *
 * The server holds a single craw_redis_storage_t* and dispatches all data
 * commands through it. v1 wires the NVS backend; the ESP32Cam-with-SD
 * variant will provide a FAT-on-SD impl with the same shape. */

typedef struct craw_redis_storage_s craw_redis_storage_t;

/* Iterator handle used by KEYS / DBSIZE. Backend-defined opaque pointer. */
typedef struct craw_redis_iter_s craw_redis_iter_t;

struct craw_redis_storage_s {
    /* Strings */
    int   (*str_get)   (const char *k, char *out, size_t *out_len, size_t max);
    int   (*str_set)   (const char *k, const char *v, size_t len);
    int   (*key_del)   (const char *k);
    /* Returns 1 if string exists, 2 if list exists, 0 otherwise. */
    int   (*key_exists)(const char *k);
    int   (*flush_all) (void);
    /* Iteration: open returns NULL on no-keys / oom. */
    craw_redis_iter_t *(*iter_open)(void);
    /* Returns 1 with key copied on hit, 0 on end, -1 on error. */
    int   (*iter_next) (craw_redis_iter_t *it, char *key_out, size_t max);
    void  (*iter_close)(craw_redis_iter_t *it);
    int   (*dbsize)    (void);
    /* Lists */
    int   (*list_lpush)(const char *k, const char *v, size_t len, int *new_len);
    int   (*list_rpush)(const char *k, const char *v, size_t len, int *new_len);
    int   (*list_lpop) (const char *k, char *out, size_t *out_len, size_t max);
    int   (*list_rpop) (const char *k, char *out, size_t *out_len, size_t max);
    int   (*list_llen) (const char *k, int *out);
    int   (*list_lindex)(const char *k, int idx, char *out, size_t *out_len, size_t max);
    /* lrange streams via emit() to avoid double-allocating. ctx is opaque. */
    int   (*list_lrange)(const char *k, int start, int stop,
                         int (*emit)(const char *v, size_t len, void *ctx),
                         void *ctx);
};

/* Built-in NVS-backed storage. Returns a static pointer; init/teardown is
 * idempotent. Values: -ERR returned if NVS unavailable. */
craw_redis_storage_t *craw_redis_storage_nvs(void);

/* ---- Server lifecycle ---- */

/* Bind + listen + spawn dispatcher. Returns 0 on success, -1 already
 * running, -2 on socket failure, -3 if storage is NULL. */
int  craw_redis_server_start(const craw_redis_config_t *cfg,
                             craw_redis_storage_t *storage);

void craw_redis_server_stop(void);
bool craw_redis_server_running(void);
void craw_redis_server_stats(craw_redis_stats_t *out);

/* Apply a profile preset to the in-memory config (does NOT auto-restart;
 * caller restarts the server to pick up changes). */
void craw_redis_profile_apply(craw_redis_profile_t p, craw_redis_config_t *cfg);

const char *craw_redis_profile_name(craw_redis_profile_t p);

/* ---- One-shot client (used by Forth `redis-do` to talk to localhost) ----
 *
 * Sends `cmd` (an inline-format command, e.g. "GET foo") to host:port,
 * formatted as a RESP array. Reads up to reply_max-1 bytes of raw reply
 * into reply (NUL-terminated). Returns 0 on success, -1 on connect, -2 on
 * send, -3 on recv. The reply is verbatim RESP — caller pretty-prints. */
int craw_redis_client_exec(const char *host, uint16_t port,
                           const char *cmd,
                           char *reply, size_t reply_max);

/* Pretty-print a RESP reply to a callback printf-like sink. Recognizes
 * the +, -, :, $, and array prefixes; multi-bulk arrays are indented. */
void craw_redis_pretty_print(const char *reply, size_t reply_len,
                             void (*sink)(const char *s, void *ctx),
                             void *ctx);

#ifdef __cplusplus
}
#endif
#endif
