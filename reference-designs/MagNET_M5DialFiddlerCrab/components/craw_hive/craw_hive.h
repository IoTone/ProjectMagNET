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
    CRAW_HIVE_MSG_UNKNOWN      = 0,
} craw_hive_msg_type_t;

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
} craw_hive_ruler_config_t;

// Start mDNS advertisement and TCP listener. Blocks only briefly; work
// happens on an internal FreeRTOS task. Returns 0 on success.
int craw_hive_ruler_start(const craw_hive_ruler_config_t *cfg);

// Stop ruler. For tests / role swaps.
void craw_hive_ruler_stop(void);

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
} craw_hive_node_config_t;

// Start the node's discover/join loop. Work happens on an internal task.
// The loop scans mDNS, connects, authenticates, and maintains the session
// with PINGs; it reconnects with backoff on failure.
int craw_hive_node_start(const craw_hive_node_config_t *cfg);

void craw_hive_node_stop(void);

craw_hive_node_state_t craw_hive_node_state(void);

// Current session_id after JOINED, or NULL otherwise.
const char *craw_hive_node_session_id(void);

#ifdef __cplusplus
}
#endif
#endif
