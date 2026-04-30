#ifndef CRAW_HIVE_H
#define CRAW_HIVE_H
#define CRAW_HIVE_VERSION "0.1.0"

// craw_hive — MagNET hive protocol v1 (Phase-4 Milestone B).
// See docs/MagNET-HiveProtocol-v1.md for the on-wire spec.
//
// This single header covers both sides:
//   - Node   (spawn/worker/etc.): craw_hive_node_*
//   - Ruler                     : craw_hive_ruler_*
// Shared codec + HMAC utilities: craw_hive_proto_*.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRAW_HIVE_PROTO_VERSION     1
#define CRAW_HIVE_DEFAULT_PORT      7447
#define CRAW_HIVE_SERVICE_TYPE      "_magnet-ruler"
#define CRAW_HIVE_SERVICE_PROTO     "_tcp"
#define CRAW_HIVE_MAX_FRAME         4096
#define CRAW_HIVE_SECRET_BYTES      32
#define CRAW_HIVE_NONCE_BYTES       16
#define CRAW_HIVE_HMAC_BYTES        32
#define CRAW_HIVE_TS_SKEW_SEC       30
#define CRAW_HIVE_HEARTBEAT_SEC     30
#define CRAW_HIVE_HIVE_ID_MAX       16
#define CRAW_HIVE_ROLE_MAX          16
#define CRAW_HIVE_ID_MAX            32

// Development fallback shared secret. Production deployments MUST replace
// this via NVS (see docs §"Shared-secret distribution"). 32 bytes.
#define CRAW_HIVE_DEV_SECRET \
    "\xA0\x8F\x19\xC3\x4B\x55\xD7\xE1\xF2\x0A\x77\x88\x99\xAA\xBB\xCC" \
    "\xDD\xEE\xFF\x11\x22\x33\x44\x55\x66\x77\x88\x99\xAA\xBB\xCC\xDD"

typedef enum {
    CRAW_HIVE_MSG_HELLO        = 1,
    CRAW_HIVE_MSG_WELCOME      = 2,
    CRAW_HIVE_MSG_REJECT       = 3,
    CRAW_HIVE_MSG_PING         = 4,
    CRAW_HIVE_MSG_ROLE_REQUEST = 5,
    CRAW_HIVE_MSG_ROLE_GRANT   = 6,
    /* Phase 4 Milestone C step 1 — generic key/value transport. KV_GET
     * and KV_PUT travel from any node to the ruler; KV_DATA / KV_NOT_FOUND
     * are the responses. Used in v1 for ad-hoc shared state and in v1+
     * for Scribe-backed role-bundle delivery (key = "bundle:<name>"). */
    CRAW_HIVE_MSG_KV_GET       = 7,
    CRAW_HIVE_MSG_KV_DATA      = 8,
    CRAW_HIVE_MSG_KV_PUT       = 9,
    CRAW_HIVE_MSG_KV_NOT_FOUND = 10,
    CRAW_HIVE_MSG_UNKNOWN      = 0,
} craw_hive_msg_type_t;

/* KV size limits. Values up to 4 KB fit comfortably in one frame
 * (CRAW_HIVE_MAX_FRAME = 4096) since the rest of the envelope is < 256
 * bytes. Bumping these is fine if MAX_FRAME is bumped accordingly. */
#define CRAW_HIVE_KV_KEY_MAX    32
#define CRAW_HIVE_KV_VALUE_MAX  3072

typedef enum {
    CRAW_HIVE_REJECT_AUTH          = 0,
    CRAW_HIVE_REJECT_HIVE_MISMATCH = 1,
    CRAW_HIVE_REJECT_FULL          = 2,
    CRAW_HIVE_REJECT_TS_SKEW       = 3,
    CRAW_HIVE_REJECT_REPLAY        = 4,
    CRAW_HIVE_REJECT_UNKNOWN_TYPE  = 5,
} craw_hive_reject_reason_t;

typedef struct {
    craw_hive_msg_type_t  type;
    char                  from[CRAW_HIVE_ID_MAX + 1];
    char                  to[CRAW_HIVE_ID_MAX + 1];
    char                  nonce_hex[CRAW_HIVE_NONCE_BYTES * 2 + 1];
    int64_t               ts;
    // Owned, heap-allocated serialized payload JSON (NULL-terminated).
    // Caller frees via craw_hive_msg_free().
    char                 *payload_json;
} craw_hive_msg_t;

// -------- proto (shared) ---------------------------------------------------

// Free heap fields on a msg struct (payload_json). Leaves stack fields.
void craw_hive_msg_free(craw_hive_msg_t *msg);

// Encode a message into a heap-allocated frame: [u32 big-endian length][json].
// Signs with the given 32-byte key. Returns frame bytes + length in *out_len.
// Caller frees *out with free().
int craw_hive_proto_encode(const craw_hive_msg_t *msg,
                           const uint8_t *key, size_t key_len,
                           uint8_t **out, size_t *out_len);

// Decode a single frame from a buffer. Verifies HMAC, timestamp skew, and
// returns a populated craw_hive_msg_t. On any failure returns a non-zero
// craw_hive_reject_reason_t cast to int. On success returns 0.
int craw_hive_proto_decode(const uint8_t *frame, size_t frame_len,
                           const uint8_t *key, size_t key_len,
                           int64_t now_epoch,
                           craw_hive_msg_t *out);

// Convenience: type<->string.
const char *craw_hive_msg_type_name(craw_hive_msg_type_t t);
craw_hive_msg_type_t craw_hive_msg_type_parse(const char *s);

// Generate a random hex nonce into buf (buf >= CRAW_HIVE_NONCE_BYTES*2 + 1).
void craw_hive_nonce_fill(char *buf);

// KV serve hook (forward typedef so it's visible to the ruler config below).
// See "ruler (continued)" section near the bottom of this file for usage.
typedef int (*craw_hive_kv_get_cb_t)(const char *key,
                                     char *value_out, size_t value_max,
                                     void *ctx);

// -------- ruler ------------------------------------------------------------

typedef struct {
    uint16_t    port;
    const char *hive_id;
    const char *ruler_id;   // e.g. "MagNET-ruler-a1b2"
    const uint8_t *secret;  // 32 bytes
    // Called on a valid HELLO. Set *accept = true to grant, fill *role_out
    // with the role to send back in WELCOME. Default-accept stub: copy
    // role_requested. Can be NULL for default behavior.
    void (*on_hello)(const char *node_id, const char *role_requested,
                     const char *hive_id, bool *accept, char *role_out,
                     size_t role_out_len, void *ctx);
    void *on_hello_ctx;
    // Optional KV override (Step 1+ — Scribe hooks its NVS via this).
    // Tried first; if it returns non-zero the ruler falls back to its
    // in-memory table. NULL = use in-memory table only.
    craw_hive_kv_get_cb_t on_kv_get;
    void *on_kv_get_ctx;
} craw_hive_ruler_config_t;

// Start mDNS advertisement and TCP listener. Blocks only briefly; work
// happens on an internal FreeRTOS task. Returns 0 on success.
int craw_hive_ruler_start(const craw_hive_ruler_config_t *cfg);

// Stop ruler. For tests / role swaps.
void craw_hive_ruler_stop(void);

// Send a ROLE_GRANT to the named connected peer. node_id matches the
// session's "from" id (e.g. "MagNET-biologic-a1b2"). bundle_key may be
// NULL for the legacy "label only" semantics, or "bundle:<name>" to
// reference a KV entry the peer should fetch + install. scribe identifies
// which scribe to fetch from ("*" = any). Returns 0 on send success,
// -1 if peer is not connected, -2 on send error.
int craw_hive_ruler_grant_role(const char *node_id,
                               const char *role,
                               const char *bundle_key,
                               const char *scribe);

// Local-table KV access on the ruler side (Forth REPL helpers + bootstrap).
// These touch the in-memory table only; the on_kv_get callback is NOT
// consulted (use this for the ruler to seed/inspect its own data).
int craw_hive_ruler_kv_get(const char *key, char *out, size_t out_len);
int craw_hive_ruler_kv_put(const char *key, const char *value);
int craw_hive_ruler_kv_iterate(int (*cb)(const char *key, const char *value, void *ctx),
                               void *ctx);

// -------- node -------------------------------------------------------------

typedef enum {
    CRAW_HIVE_NODE_OFFLINE    = 0,
    CRAW_HIVE_NODE_DISCOVER   = 1,
    CRAW_HIVE_NODE_CONNECTING = 2,
    CRAW_HIVE_NODE_JOINED     = 3,
    CRAW_HIVE_NODE_BACKOFF    = 4,
} craw_hive_node_state_t;

typedef struct {
    const char    *node_id;        // "MagNET-biologic-a1b2"
    const char    *hive_id;        // "beehive-1"
    const char    *role_requested; // "spawn"
    const char   **caps;           // NULL-terminated, e.g. {"led","button",NULL}
    const char    *chip;           // "ESP32-C3"
    const char    *fw;             // "0.1.0"
    const uint8_t *secret;         // 32 bytes
    // State transition callback. May be NULL.
    void (*on_state)(craw_hive_node_state_t state, const char *info, void *ctx);
    void  *on_state_ctx;
    // Phase 4 Milestone C step 3: ROLE_GRANT received from the ruler.
    // role        — the new role label (always non-NULL).
    // bundle_key  — KV key naming the bundle to fetch ("bundle:spy"), or NULL
    //               to keep the v1 "label only" semantics.
    // scribe      — explicit scribe id to fetch from, or "*" / NULL = any.
    // App typically responds by spawning a worker task that calls
    // craw_hive_node_kv_get(bundle_key) and then
    // craw_role_bundle_install_from_json(value). Don't do KV_GET inline —
    // this callback runs on the receive-loop task and would deadlock.
    void (*on_role_grant)(const char *role, const char *bundle_key,
                          const char *scribe, void *ctx);
    void  *on_role_grant_ctx;
} craw_hive_node_config_t;

// Start the node's discover/join loop. Work happens on an internal task.
// The loop scans mDNS, connects, authenticates, and maintains the session
// with PINGs; it reconnects with backoff on failure.
int craw_hive_node_start(const craw_hive_node_config_t *cfg);

void craw_hive_node_stop(void);

craw_hive_node_state_t craw_hive_node_state(void);

// Current session_id after JOINED, or NULL otherwise.
const char *craw_hive_node_session_id(void);

// KV: send KV_GET to the ruler, block waiting for KV_DATA / KV_NOT_FOUND.
// Returns 0 on found (value copied to value_out, NUL-terminated),
// 1 on not_found, -1 on error (no session, send fail), -2 on timeout.
// Only one KV request per node is in flight at a time (serialized internally).
int craw_hive_node_kv_get(const char *key,
                          char *value_out, size_t value_max,
                          int timeout_ms);

// KV: fire-and-forget KV_PUT to the ruler. Returns 0 on send success.
// No ack message in v1 — assume the ruler stored it.
int craw_hive_node_kv_put(const char *key, const char *value);

// craw_hive_kv_get_cb_t is declared near the top of this file. Optional
// KV-serve hook: ruler config can install this to override the in-memory
// table. Called on KV_GET; if it returns 0 with value populated, that
// value is sent. If it returns non-zero, ruler falls back to its own
// table. Use case: Scribe wires its NVS-backed kv-store via this hook.

#ifdef __cplusplus
}
#endif
#endif
