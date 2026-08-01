/*
 * magnet.h — MagNET Hanasu node core (E-Phase B: plaintext mesh chat)
 *
 * This is the C layer that owns the protocol, transport, and (later) crypto.
 * The Forth engine sits ABOVE this as a control/scripting surface and calls
 * the same mn_* functions the HCP dispatcher calls (design proposal §12).
 *
 * E-PHASE B SCOPE: v2.1 envelope (plaintext), unified single-role Thread node,
 * CoAP /magnet resource, multicast + unicast chat, peer table. Crypto,
 * channels-by-passphrase, and identity keys remain stubs until E-Phase D.
 */
#ifndef MAGNET_H
#define MAGNET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Transport char I/O (provided by main: USB-serial-JTAG, later BLE/WS) ---- */
typedef int  (*mn_getc_fn)(void);
typedef void (*mn_putc_fn)(int);

/* Lifecycle states (design proposal §4.6 / §11.3). */
typedef enum {
    MN_BOOTING = 0,
    MN_CONFIGURING,
    MN_ATTACHING,
    MN_READY,
    MN_DEGRADED,
} mn_state_t;

/* ---- Bringup (call order matters — see main.c / §12.5) ---- */
void mn_core_init(void);                 /* peer table, dedup cache, event pump */
void mn_register_forth_vocab(void);      /* register mn-* FFI words with ESPIDFORTH    */
void mn_link_start(mn_getc_fn g, mn_putc_fn p); /* start dispatcher + TX writer (HCP)  */
void mn_openthread_start(void);          /* bring up esp_openthread (no-op if disabled) */

/* ---- State ---- */
mn_state_t  mn_get_state(void);
void        mn_set_state(mn_state_t s);  /* emits an !STATE event */
const char *mn_state_name(mn_state_t s);

/* ---- Serialized output (the ONE writer all lines go through) ---- */
/* Thread-safe: takes the TX mutex, writes the whole line + CRLF atomically.
 * Used for HCP responses (+/-), async events (!), and comments (#). This is
 * what guarantees an event never interleaves mid-line with a response.
 * MUST NOT be called from OpenThread/lwIP callback context — mesh-side code
 * posts to the event pump instead (mn_post_*). */
void mn_write_line(const char *line);
void mn_emit_event(const char *fmt, ...);   /* convenience: formats then mn_write_line */

/* ---- Identity (E-B: derived from EUI-64; E-D: SHA256(Ed25519 pk)[0:4]) ---- */
void           mn_core_set_device_id(const uint8_t id[4]);
const uint8_t *mn_device_id(void);

/* ---- Core operations (shared by HCP verbs AND Forth FFI words) ---- */
/* mn_chat is token-bucket rate limited (burst 8, refill 10/s — §4.9);
 * returns -4 when limited (HCP: -ERR E_RATE_LIMITED). */
int  mn_chat(const char *msg, size_t len);                   /* multicast, Type 0 */
int  mn_dm(const char *peer_ipv6, const char *msg, size_t len); /* unicast CON    */
void mn_status_line(char *buf, size_t cap);   /* "state=… role=… peers=… id=…"  */
void mn_whoami_line(char *buf, size_t cap);   /* "id=… name=… fw=…"             */
void mn_peers_print(void);

/* ---- E-C: identity name, mode, subscriptions, DEGRADED queue ---- */
void        mn_name_set(const char *name);    /* ≤16 chars; persists to NVS and
                                                 announces to the mesh if READY */
const char *mn_name_get(void);
void        mn_terse_set(bool terse);         /* TERSE: mn_write_line drops '#' */
int         mn_sub_update(const char *csv, bool subscribe); /* event classes:
                                                 chat,dm,cmd,state,peer,role,
                                                 heartbeat,warn,all; -1 = unknown */
int         mn_queue_chat(const char *dst_ipv6_or_null,     /* DEGRADED queue,  */
                          const char *msg, size_t len);     /* max 4; returns   */
                                                            /* depth or -1 full */

/* ---- E-D: encrypted channels (§11.1) ---- */
/* Derive from credential (Path A/B/C auto-detected), persist root to NVS,
 * switch the live channel + multicast group. May block ~1 s for Path B.
 * Fills info with "name=… selector=…". 0 = ok. */
int  mn_channel_set(const char *cred, size_t len, char *info, size_t cap);
void mn_channel_info(char *buf, size_t cap);      /* public info only (§11.3.5) */
const uint8_t *mn_channel_mcast_suffix(void);     /* 4 bytes, for OT bringup    */
bool mn_channel_is_default(void);   /* still on the well-known "magnet" channel */

/* ---- E-D part 2: identity + admin (deterministic ECDSA P-256) ---- */
const uint8_t *mn_pubkey(void);              /* 65-byte uncompressed point */
int  mn_admin_add(const uint8_t pub65[65]);  /* allow-list, NVS, max 4     */
void mn_admin_list_print(void);
int  mn_rotate(void);                        /* signed system/rotate bcast */

/* ---- E-F: BLE-GATT HCP binding (§11.2.1), PROVISIONING-ONLY ----
 * Decision §12.9 Q3: BLE is torn down once a channel is provisioned, so the
 * 2.4 GHz front end and ~40-60 KB of NimBLE RAM go back to Thread. Compiled
 * out unless MN_ENABLE_BLE=1. */
int  mn_ble_start(void);
int  mn_ble_stop(void);
bool mn_ble_running(void);
bool mn_ble_link_secure(void);  /* bonded+encrypted right now */
void mn_ble_notify(const char *line);   /* mirror one HCP line to the client */

/* Feed one complete line to the HCP dispatcher from ANY transport (serial,
 * BLE, later WebSocket) — the grammar is transport-independent (§11.2). */
void mn_link_feed_line(const char *line);

/* ---- E-E: Forth automation hooks + script persistence (§12.3) ----
 * Hooks are registered by WORD NAME (the stub engine exposes no execution
 * tokens; spec's `( xt -- )` form returns with the full ESP32forth port, E-G).
 * The hook word is invoked ON THE PUMP TASK — never in OT/lwIP callback
 * context — with the message pushed as ( c-addr u ) plus the sender id.
 *   kind 0 = chat, 1 = m2m cmd */
int  mn_hook_set(int kind, const char *word, size_t len);
void mn_hooks_print(void);

/* Serialized Forth execution: the interpreter is NOT reentrant, so the REPL
 * dispatcher and the hook invoker must not call forth_eval concurrently. */
void mn_forth_exec(const char *line);

/* Boot script: persisted Forth source, run once after vocab registration. */
int  mn_script_save(const char *src, size_t len);   /* NVS, ≤1 KB */
void mn_script_show(void);
int  mn_script_run(void);                           /* run the saved script now */

/* Switch the subscribed multicast group at runtime (magnet_ot.c). */
int  mn_ot_set_mcast(const uint8_t suffix[4]);

/* ---- Diagnostics / test surface (Forth: mn-sysinfo …; HCP: SYSINFO …) ---- */
void mn_sysinfo_print(void);             /* chip, IDF, heap, forth heap, uptime */
void mn_bench(void);                     /* envelope codec + TX-call latency    */
int  mn_selftest(void);                  /* env roundtrip + pump + CoAP loopback;
                                            0 = all pass (skips don't fail)     */
void mn_heartbeat_set(uint32_t secs);    /* !HEARTBEAT interval, 0 = off (§4.6) */
void mn_stats_print(void);               /* tx/rx counters since boot or reset  */
void mn_stats_reset(void);
int  mn_stress_start(uint32_t secs, uint32_t payload_len); /* saturation burst:
                                            spawns a task that multicasts marked
                                            chat frames back-to-back; receivers
                                            count them silently. 0=started,
                                            -1=already running, -2=bad args    */

/* ---- Event pump (§12.4) ----
 * OpenThread callbacks run with the OT lock held; taking the TX mutex there
 * can ABBA-deadlock against a Forth/HCP task that holds the TX mutex while
 * sending (which takes the OT lock). So mesh-side code posts into a queue and
 * a pump task does the core processing + emission. */
void mn_post_rx(const uint8_t *data, size_t len,
                const char *src_ipv6, bool was_multicast);
void mn_post_role(const char *role_name);

/* ---- Mesh transport (implemented by magnet_ot.c; stubbed when OT disabled) ---- */
/* dst_ipv6 == NULL → the default channel multicast group. */
int         mn_ot_send(const uint8_t *buf, size_t len,
                       const char *dst_ipv6, bool confirmable);
const char *mn_ot_role_name(void);
void        mn_mesh_print(void);         /* Thread detail: partition, RLOC, EID,
                                            channel/PAN, neighbors w/ RSSI      */
int         mn_ot_local_eid(char *buf, size_t cap);  /* mesh-local EID string;
                                            <0 if radio not up (loopback test) */

#ifdef __cplusplus
}
#endif

#endif /* MAGNET_H */
