/*
 * magnet_core.c — state, serialized TX writer, envelope send/receive, peers.
 *
 * The serialized TX writer is the load-bearing piece: every line out of the
 * node (HCP responses, async events, Forth output) funnels through one mutex
 * so output can never interleave mid-line (design proposal §12.4).
 *
 * The event pump is the other load-bearing piece: OpenThread callbacks post
 * into a queue; the pump task does dedup/peer-tracking/emission. Mesh-side
 * code never touches the TX mutex directly (deadlock discipline, see magnet.h).
 */
#include "magnet.h"
#include "magnet_envelope.h"
#include "magnet_crypto.h"
#include "magnet_bot.h"
#include "magnet_xfer.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "forth_core.h"
#include "sdkconfig.h"

/* MN_FW_VERSION lives in magnet.h (H9: single-source the version) */
#define MN_NAME_MAX   16

/* E-D: the active channel — everything (selector, mcast, key) derives from
 * the credential's root_secret (§11.1). Default = "magnet" (well-known,
 * !WARN insecure per §11.5), derived once and cached in NVS. */
static mn_channel_t s_chan;
#define MN_CHANNEL_NAME (s_chan.name)

/* E-D part 2: device identity (deterministic ECDSA P-256) + admin allow-list */
static uint8_t s_pub[65];
#define MN_ADMIN_MAX 4
static struct { bool used; uint8_t pub[65]; } s_admins[MN_ADMIN_MAX];
static uint8_t s_prev_epoch_key[16];
static uint8_t s_epoch = 0;

/* E-E: automation hooks (by word name) + the Forth engine mutex. The engine
 * has global state (stacks, dict pointer) and is NOT reentrant, so every
 * forth_eval() in the firmware goes through mn_forth_exec(). */
#define MN_HOOK_WORD_MAX 32
static char s_hook_chat[MN_HOOK_WORD_MAX + 1];
static char s_hook_cmd[MN_HOOK_WORD_MAX + 1];
static SemaphoreHandle_t s_forth_mutex = NULL;

/* ---- host-set wall clock + mesh clock distribution ----
 * Anchored to esp_timer rather than settimeofday: newlib's strftime/tzset path
 * measured +8.8 KB of flash to print eight characters. Behaviour (and the
 * stratum rules) lives further down, next to mn_time_set(). */
#define MN_TIME_MAX_STRATUM   4          /* refuse anything this far from a host */
#define MN_TIME_REFRESH_MS    (15 * 60 * 1000)   /* stratum-0 re-announce */
#define MN_TIME_STRATUM_NONE  0xFF

static bool     s_clock_set = false;
static int64_t  s_epoch_base = 0;     /* UTC epoch at the moment it was set  */
static int64_t  s_uptime_at_set_us = 0;
static int      s_tz_offset_min = 0;  /* minutes east of UTC, e.g. JST = 540 */
static uint8_t  s_time_stratum = MN_TIME_STRATUM_NONE;
static uint8_t  s_time_src[4];        /* who we learned it from (self if s0) */
static int64_t  s_time_learned_us = 0;
static TimerHandle_t s_time_timer = NULL;  /* jittered reply + periodic refresh */

/* H7: anchor-role persistence (docs/MESH-TIME.md §the stratum ratchet).
 * The CLOCK is deliberately never persisted — a node powered off for an
 * unknown interval would announce a confidently wrong time at stratum 0.
 * What survives reboot is the ROLE: "I was the anchor." An ex-anchor with
 * no clock says so loudly and keeps asking to be re-seeded instead of
 * letting the mesh ratchet toward MN_TIME_MAX_STRATUM in silence. */
static bool s_was_anchor    = false;
static bool s_anchor_warned = false;

static void anchor_persist(bool on) {
    nvs_handle_t h;
    if (nvs_open("magnet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "t_anchor", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void anchor_load(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("magnet", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "t_anchor", &v);
        nvs_close(h);
    }
    s_was_anchor = (v != 0);
}

static int  time_apply(int64_t epoch_secs, int tz_offset_min, uint8_t stratum,
                       const uint8_t src[4]);
static void time_reply_schedule(void);
static void time_timer_cb(TimerHandle_t t);

static mn_state_t   s_state = MN_BOOTING;
static mn_putc_fn   s_putc  = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;

static uint8_t  s_device_id[4] = {0xde, 0xad, 0xbe, 0xef};
static uint32_t s_counter      = 0;   /* E-B: per-boot random base; E-D: NVS blocks */

/* ---- E-C link/host state ---- */
static char     s_name[MN_NAME_MAX + 1] = "-";
static bool     s_terse = false;

/* event classes for SUB/UNSUB (§11.3 point 7) */
enum {
    MN_EC_CHAT = 1 << 0, MN_EC_DM = 1 << 1, MN_EC_CMD = 1 << 2,
    MN_EC_STATE = 1 << 3, MN_EC_PEER = 1 << 4, MN_EC_XFER = 1 << 5,
    MN_EC_ROLE = 1 << 6, MN_EC_HEARTBEAT = 1 << 7, MN_EC_WARN = 1 << 8,
    MN_EC_ALL = 0x1FF,
};
static uint16_t s_ev_mask = MN_EC_ALL;

/* class-gated event emission — subscription filters delivery to the host,
 * never the mesh processing itself (counters/peers still update) */
static void emit_class(uint16_t cls, const char *fmt, ...) {
    if (!(s_ev_mask & cls)) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mn_write_line(buf);
}

/* ---- called by mn_link_start once the transport putc is known ---- */
void mn_core_set_putc(mn_putc_fn p) { s_putc = p; }

void mn_core_set_device_id(const uint8_t id[4]) { memcpy(s_device_id, id, 4); }
const uint8_t *mn_device_id(void) { return s_device_id; }

const char *mn_state_name(mn_state_t s) {
    switch (s) {
        case MN_BOOTING:     return "BOOTING";
        case MN_CONFIGURING: return "CONFIGURING";
        case MN_ATTACHING:   return "ATTACHING";
        case MN_READY:       return "READY";
        case MN_DEGRADED:    return "DEGRADED";
        default:             return "UNKNOWN";
    }
}

mn_state_t mn_get_state(void) { return s_state; }

void mn_set_state(mn_state_t s) {
    s_state = s;
    emit_class(MN_EC_STATE, "!STATE %s", mn_state_name(s));
}

/* ---- The single serialized writer. Lock the WHOLE line. ----
 * RECURSIVE mutex: the link dispatcher holds it across forth_eval() (FORTH
 * mode), and mn-* words called from Forth print through here — a plain mutex
 * would self-deadlock on the first `mn-status` at the ok> prompt. */
void mn_write_line(const char *line) {
    if (s_terse && line[0] == '#') return;   /* MODE TERSE: drop comment lines */
    mn_ble_notify(line);                     /* mirror to a BLE client if any */
    if (!s_putc) return;
    if (s_tx_mutex) xSemaphoreTakeRecursive(s_tx_mutex, portMAX_DELAY);
    for (const char *c = line; *c; ++c) s_putc(*c);
    s_putc('\r');
    s_putc('\n');
    if (s_tx_mutex) xSemaphoreGiveRecursive(s_tx_mutex);
}

void mn_terse_set(bool terse) { s_terse = terse; }

int mn_sub_update(const char *csv, bool subscribe) {
    static const struct { const char *name; uint16_t bit; } MAP[] = {
        {"chat", MN_EC_CHAT}, {"dm", MN_EC_DM}, {"cmd", MN_EC_CMD},
        {"state", MN_EC_STATE}, {"peer", MN_EC_PEER}, {"xfer", MN_EC_XFER},
        {"role", MN_EC_ROLE}, {"heartbeat", MN_EC_HEARTBEAT},
        {"warn", MN_EC_WARN}, {"all", MN_EC_ALL},
    };
    char tok[16];
    const char *p = csv;
    while (*p) {
        size_t i = 0;
        while (*p && *p != ',' && i < sizeof(tok) - 1) tok[i++] = *p++;
        tok[i] = '\0';
        if (*p == ',') p++;
        uint16_t bit = 0;
        for (size_t m = 0; m < sizeof(MAP) / sizeof(MAP[0]); m++)
            if (!strcmp(tok, MAP[m].name)) { bit = MAP[m].bit; break; }
        if (!bit) return -1;
        if (subscribe) s_ev_mask |= bit;
        else           s_ev_mask &= ~bit;
    }
    return 0;
}

void mn_emit_event(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mn_write_line(buf);
}

/* Expose the mutex so the dispatcher can hold it across a whole forth_eval()
 * (keeping Forth output atomic vs. async events). */
SemaphoreHandle_t mn_tx_mutex(void) { return s_tx_mutex; }

/* ================= peers (E-B: learned from received envelopes) ============ */
/* E-G (§11.6): sized for the 32+ node target — an undersized LRU table
 * thrashes at scale, which silently re-announces PEER_JOIN and re-admits
 * replayed counters. 40 slots ≈ 3 KB; cheap insurance. */
#define MN_PEER_MAX 40

typedef struct {
    bool     used;
    uint8_t  id[4];
    char     ipv6[46];
    char     name[MN_NAME_MAX + 1];
    uint32_t last_seen_ticks;
} mn_peer_t;

static mn_peer_t s_peers[MN_PEER_MAX];

static mn_peer_t *peer_find(const uint8_t id[4]) {
    for (int i = 0; i < MN_PEER_MAX; i++)
        if (s_peers[i].used && !memcmp(s_peers[i].id, id, 4)) return &s_peers[i];
    return NULL;
}

static const char *peer_name(const uint8_t id[4]) {
    mn_peer_t *p = peer_find(id);
    return (p && p->name[0]) ? p->name : "-";
}

static void peer_seen(const uint8_t id[4], const char *ipv6) {
    mn_peer_t *p = peer_find(id);
    if (p) {
        strlcpy(p->ipv6, ipv6, sizeof(p->ipv6));
        p->last_seen_ticks = xTaskGetTickCount();
        return;
    }
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < MN_PEER_MAX; i++) {
        if (!s_peers[i].used && free_slot < 0) free_slot = i;
        if (s_peers[i].last_seen_ticks < s_peers[oldest].last_seen_ticks) oldest = i;
    }
    int slot = (free_slot >= 0) ? free_slot : oldest;
    s_peers[slot].used = true;
    memcpy(s_peers[slot].id, id, 4);
    strlcpy(s_peers[slot].ipv6, ipv6, sizeof(s_peers[slot].ipv6));
    s_peers[slot].name[0] = '\0';
    s_peers[slot].last_seen_ticks = xTaskGetTickCount();
    if (free_slot >= 0) {
        emit_class(MN_EC_PEER, "!PEER_JOIN %02x%02x%02x%02x - %s",
                   id[0], id[1], id[2], id[3], ipv6);
    }
}

/* announce (Type 1, ns 0x00 system, cmd 0x02) carries the display name so
 * !CHAT can show names instead of hex ids (resolves Open Q2 at E-C level) */
static void peer_set_name(const uint8_t id[4], const char *name, size_t len) {
    mn_peer_t *p = peer_find(id);
    if (!p) return;
    bool first = (p->name[0] == '\0');
    size_t n = len <= MN_NAME_MAX ? len : MN_NAME_MAX;
    memcpy(p->name, name, n);
    p->name[n] = '\0';
    if (first) {
        emit_class(MN_EC_PEER, "!PEER_JOIN %02x%02x%02x%02x %s %s",
                   id[0], id[1], id[2], id[3], p->name, p->ipv6);
    }
}

static int peer_count(void) {
    int n = 0;
    for (int i = 0; i < MN_PEER_MAX; i++) if (s_peers[i].used) n++;
    return n;
}

/* ============ replay protection (§11.1.6 high-water table) =================
 * Senders use PERSISTENT monotonic counters (NVS block-reserve), so a frame
 * with counter ≤ the sender's high-water mark is a replay/duplicate → drop.
 * Unknown sender → trust-on-first-use. LRU-bounded.
 * E-G: sized with the peer table — an evicted entry re-admits old counters. */
#define MN_DEDUP_MAX 40

static struct { bool used; uint8_t id[4]; uint32_t hi; uint32_t ticks; } s_dedup[MN_DEDUP_MAX];

static bool dedup_seen(const uint8_t id[4], uint32_t counter) {
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < MN_DEDUP_MAX; i++) {
        if (s_dedup[i].used && !memcmp(s_dedup[i].id, id, 4)) {
            if (counter <= s_dedup[i].hi) return true;       /* replay */
            s_dedup[i].hi = counter;
            s_dedup[i].ticks = xTaskGetTickCount();
            return false;
        }
        if (!s_dedup[i].used && free_slot < 0) free_slot = i;
        if (s_dedup[i].ticks < s_dedup[oldest].ticks) oldest = i;
    }
    int slot = free_slot >= 0 ? free_slot : oldest;          /* TOFU */
    s_dedup[slot].used = true;
    memcpy(s_dedup[slot].id, id, 4);
    s_dedup[slot].hi = counter;
    s_dedup[slot].ticks = xTaskGetTickCount();
    return false;
}

/* forward decls (defined with the send path below; used by the pump/init) */
static void announce_name(void);
static void announce_schedule(void);
static void queue_drain(void);
static void name_load(void);
static void counter_load(void);
static void chan_load(void);
static void admins_load(void);
static void script_load(void);
static void hook_invoke(const char *word, const uint8_t *payload, size_t len);
static bool admin_verify(const uint8_t *signed_part, size_t len, const uint8_t sig[64]);
static void epoch_apply(uint8_t e);
static int  send_frame_ex(uint8_t type, const uint8_t *payload, size_t len,
                          const char *dst, bool con, bool admin,
                          uint8_t extra_flags);

/* ==================== event pump (§12.4) =================================== */
typedef enum { MN_EVT_RX, MN_EVT_ROLE, MN_EVT_HEARTBEAT,
               MN_EVT_ANNOUNCE, MN_EVT_NOTE, MN_EVT_TIMEPUSH,
               MN_EVT_XFER_TICK } mn_evt_kind_t;

typedef struct {
    mn_evt_kind_t kind;
    /* RX (data doubles as the NOTE text) */
    uint16_t len;
    bool     was_multicast;
    bool     from_recent;    /* catch-up replay: src is the server, not sender */
    char     src[46];
    uint8_t  data[MN_ENV_MAX_FRAME];
    /* ROLE */
    char     role[12];
} mn_evt_t;

static QueueHandle_t s_evt_q = NULL;

void mn_post_rx(const uint8_t *data, size_t len,
                const char *src_ipv6, bool was_multicast) {
    if (!s_evt_q || len > MN_ENV_MAX_FRAME) return;
    static mn_evt_t evt;                 /* posted by ONE task (OT mainloop) */
    evt.kind = MN_EVT_RX;
    evt.len = (uint16_t)len;
    evt.was_multicast = was_multicast;
    evt.from_recent = false;
    strlcpy(evt.src, src_ipv6, sizeof(evt.src));
    memcpy(evt.data, data, len);
    xQueueSend(s_evt_q, &evt, 0);        /* full queue → drop (mesh is lossy anyway) */
}

void mn_post_rx_recent(const uint8_t *data, size_t len) {
    if (!s_evt_q || len > MN_ENV_MAX_FRAME) return;
    static mn_evt_t evt;                 /* OT mainloop is the only poster too */
    evt.kind = MN_EVT_RX;
    evt.len = (uint16_t)len;
    evt.was_multicast = true;            /* recent ring holds multicast chat */
    evt.from_recent = true;
    strlcpy(evt.src, "(recent)", sizeof(evt.src));
    memcpy(evt.data, data, len);
    xQueueSend(s_evt_q, &evt, 0);
}

void mn_post_role(const char *role_name) {
    if (!s_evt_q) return;
    static mn_evt_t evt;                 /* same single-poster discipline */
    evt.kind = MN_EVT_ROLE;
    strlcpy(evt.role, role_name, sizeof(evt.role));
    xQueueSend(s_evt_q, &evt, 0);
}

void mn_post_note(const char *fmt, ...) {
    if (!s_evt_q) return;
    static mn_evt_t evt;                 /* OT mainloop is the only poster */
    evt.kind = MN_EVT_NOTE;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf((char *)evt.data, sizeof(evt.data), fmt, ap);
    va_end(ap);
    xQueueSend(s_evt_q, &evt, 0);
}

/* E-H: the xfer tick timer (timer-daemon task) posts here; the pump task
 * runs mn_xfer_tick(). Kind-only event — safe to share a static. */
void mn_post_xfer_tick(void) {
    if (!s_evt_q) return;
    static mn_evt_t evt;                 /* timer-daemon task is the only poster */
    evt.kind = MN_EVT_XFER_TICK;
    xQueueSend(s_evt_q, &evt, 0);
}

/* ---- traffic counters (STATS / STRESS) ---- */
static struct {
    uint32_t tx_try, tx_ok, tx_err;
    uint32_t rx_msgs, rx_dup, rx_err;
    uint64_t tx_bytes, rx_bytes;
} s_stats;

/* Stress frames are chat-type but carried with this payload prefix so
 * receivers count them without emitting a 500-byte !CHAT per frame. */
static const uint8_t MN_STRESS_MARK[3] = { 0x01, 'S', 'T' };

/* selftest handshake: handle_rx gives this when the armed magic PING lands */
static SemaphoreHandle_t s_selftest_sem = NULL;
static volatile bool     s_selftest_armed = false;
static const uint8_t     MN_SELFTEST_MAGIC[8] = { 'M','N','S','E','L','F','T','S' };

static void handle_rx(const mn_evt_t *evt) {
    mn_envelope_t env;
    if (mn_env_unpack(&env, evt->data, evt->len) != 0) { s_stats.rx_err++; return; }

    /* PINGs are handled BEFORE the self-drop — the loopback selftest sends a
     * magic PING to our own mesh-local EID and expects to see it back. */
    if (env.type == MN_T_PING) {
        if (s_selftest_armed && env.payload_len == sizeof(MN_SELFTEST_MAGIC) &&
            !memcmp(env.payload, MN_SELFTEST_MAGIC, sizeof(MN_SELFTEST_MAGIC))) {
            s_selftest_armed = false;
            xSemaphoreGive(s_selftest_sem);
        }
        return;                                   /* pings never chat-emit */
    }

    /* Self-frames drop — except Type 6, whose CON-unicast frames may
     * legitimately target our own ML-EID (single-node loopback test). */
    if (!memcmp(env.sender_id, s_device_id, 4) &&
        env.type != MN_T_XFER6) return;
    if (env.selector != s_chan.selector) return;              /* not our channel */

    /* §11.1.7: ADMIN frames must carry a valid allow-listed signature over
     * everything before the 64-byte trailer — checked before decrypt/exec */
    bool is_admin = false;
    if (env.flags & MN_F_SIGNED) {
        if (env.payload_len <= 64) { s_stats.rx_err++; return; }
        size_t signed_len = evt->len - 64;
        if (!(env.flags & MN_F_ADMIN) ||
            !admin_verify(evt->data, signed_len, evt->data + signed_len)) {
            s_stats.rx_err++;
            return;
        }
        env.payload_len -= 64;
        is_admin = true;
    }

    /* §11.1.5: decrypt + authenticate BEFORE any stateful processing */
    uint8_t pt[MN_ENV_MAX_FRAME];
    if (env.flags & MN_F_ENCRYPTED) {
        if (env.payload_len <= 8) { s_stats.rx_err++; return; }
        size_t ct_len = env.payload_len - 8;
        uint8_t nonce[13];
        mn_nonce_build(nonce, env.sender_id, env.counter, env.epoch, env.selector);
        const uint8_t *key = (env.epoch == s_epoch) ? s_chan.epoch_key :
                             (env.epoch == (uint8_t)(s_epoch - 1)) ? s_prev_epoch_key : NULL;
        if (!key || mn_aead_decrypt(key, nonce, evt->data,
                                    env.payload, ct_len, pt,
                                    env.payload + ct_len) != 0) {
            s_stats.rx_err++;                                 /* bad MIC/epoch */
            return;
        }
        env.payload = pt;
        env.payload_len = ct_len;
    } else if (s_chan.set) {
        return;      /* plaintext on an encrypted channel → drop (E-D strict) */
    }

    if (dedup_seen(env.sender_id, env.counter)) { s_stats.rx_dup++; return; }

    s_stats.rx_msgs++;
    s_stats.rx_bytes += env.payload_len;
    /* catch-up frames arrive FROM the serving node — its address says nothing
     * about where the original sender lives, so don't teach the peer table.
     * Loopback Type 6 frames from ourselves stay out of it too. */
    if (!evt->from_recent && memcmp(env.sender_id, s_device_id, 4))
        peer_seen(env.sender_id, evt->src);

    /* stress frames: count only, never emit (they'd flood the console) */
    if (env.type == MN_T_CHAT && env.payload_len >= sizeof(MN_STRESS_MARK) &&
        !memcmp(env.payload, MN_STRESS_MARK, sizeof(MN_STRESS_MARK))) {
        return;
    }

    /* E-G: accepted multicast chat goes into the catch-up ring, raw wire bytes
     * (still ciphertext — serving it reveals nothing the air didn't) */
    if (env.type == MN_T_CHAT && evt->was_multicast && !evt->from_recent)
        mn_recent_store(evt->data, evt->len);

    char idhex[9];
    snprintf(idhex, sizeof(idhex), "%02x%02x%02x%02x",
             env.sender_id[0], env.sender_id[1], env.sender_id[2], env.sender_id[3]);

    switch (env.type) {
    case MN_T_CHAT: {
        char text[MN_ENV_MAX_FRAME];
        size_t n = env.payload_len < sizeof(text) - 1 ? env.payload_len : sizeof(text) - 1;
        memcpy(text, env.payload, n);
        text[n] = '\0';
        if (evt->was_multicast)
            emit_class(MN_EC_CHAT, "!CHAT %s %s %s %s", MN_CHANNEL_NAME, idhex,
                       peer_name(env.sender_id), text);
        else
            emit_class(MN_EC_DM, "!DM %s %s %s", idhex, peer_name(env.sender_id), text);
        hook_invoke(s_hook_chat, env.payload, env.payload_len);   /* E-E */
        /* Bot mode runs AFTER the host event so the console/app always shows
         * the stimulus before the response, and after the hook so a Forth
         * script still sees every frame whether or not a bot is armed. */
        mn_bot_on_chat(env.sender_id, peer_name(env.sender_id), text,
                       (size_t)n, env.flags, evt->was_multicast);
        break;
    }
    case MN_T_M2M_CMD: {
        /* §4.4: [0]=ns [1]=cmd [2..3]=param_len BE [4..]=params */
        if (env.payload_len < 4) break;
        uint8_t  ns  = env.payload[0], cmd_id = env.payload[1];
        uint16_t plen = (uint16_t)((env.payload[2] << 8) | env.payload[3]);
        if ((size_t)plen + 4 > env.payload_len) break;
        if (ns == 0x00 && cmd_id == 0x02) {          /* system/announce: name */
            peer_set_name(env.sender_id, (const char *)&env.payload[4], plen);
        } else if (ns == 0x00 && cmd_id == 0x03) {   /* system/rotate (§11.1.8) */
            if (is_admin && plen == 1) epoch_apply(env.payload[4]);
        } else if (ns == 0x00 && cmd_id == 0x04 && plen == 11) {  /* system/time */
            int64_t epoch = 0;
            for (int i = 0; i < 8; i++)
                epoch = (epoch << 8) | env.payload[4 + i];
            int tz = (int16_t)((env.payload[12] << 8) | env.payload[13]);
            uint8_t from_stratum = env.payload[14];
            uint8_t mine = (uint8_t)(from_stratum + 1);

            /* Hearing ANY announce cancels a reply we had queued — someone
             * beat us to it, which is the whole point of the jitter. */
            if (s_time_timer) xTimerStop(s_time_timer, 0);

            bool take;
            if (from_stratum >= MN_TIME_MAX_STRATUM) take = false;
            else if (!s_clock_set)              take = true;
            else if (mine <  s_time_stratum)    take = true;   /* better source */
            else if (mine >  s_time_stratum)    take = false;  /* worse — ignore */
            /* Equal stratum: accept a refresh from the source we already
             * follow (memcmp == 0), otherwise break the tie on lowest sender
             * id. Without a deterministic rule here two equal peers re-adopt
             * each other's clock forever, each hop adding the announce latency
             * as drift. */
            else take = memcmp(env.sender_id, s_time_src, 4) <= 0;

            if (take && time_apply(epoch, tz, mine, env.sender_id) == 0) {
                char info[96];
                mn_time_info(info, sizeof(info));
                mn_emit_event("# time adopted from %s (%s)", idhex, info);
                /* H7: an ex-anchor now following the mesh is a degraded
                 * state the operator should hear about exactly once. */
                if (s_was_anchor && mine > 0 && !s_anchor_warned) {
                    s_anchor_warned = true;
                    emit_class(MN_EC_WARN,
                        "!WARN time-anchor-degraded stratum=%u — ex-anchor "
                        "running on mesh time; re-seed with TIME SET to "
                        "restore stratum 0", mine);
                }
            }
            /* H7: hearing a DIFFERENT node announce at stratum 0 means the
             * host moved the seed — release the persisted anchor role. */
            if (from_stratum == 0 && s_was_anchor &&
                memcmp(env.sender_id, s_device_id, 4) != 0) {
                s_was_anchor = false;
                anchor_persist(false);
                mn_emit_event("# time anchor moved to %s — persisted anchor role released", idhex);
            }
            /* A stratum-0 node keeps its own refresh cadence running even when
             * it ignores a peer — it is the anchor, not a follower. */
            if (s_time_stratum == 0 && s_time_timer)
                xTimerChangePeriod(s_time_timer,
                                   pdMS_TO_TICKS(MN_TIME_REFRESH_MS), 0);
        } else if (ns == 0x00 && cmd_id == 0x05) {   /* system/time request */
            time_reply_schedule();
        } else {
            emit_class(MN_EC_CMD, "!CMD %s %s %u %u len=%u",
                       MN_CHANNEL_NAME, idhex, ns, cmd_id, plen);
            hook_invoke(s_hook_cmd, &env.payload[4], plen);       /* E-E */
        }
        break;
    }
    case MN_T_XFER6:                              /* E-H extended transfer */
        mn_xfer_on_rx(env.sender_id, env.payload, env.payload_len, evt->src);
        break;
    default:
        mn_emit_event("# rx type=%u from=%s len=%u (unhandled in E-C)",
                      env.type, idhex, (unsigned)env.payload_len);
        break;
    }
}

static void pump_task(void *arg) {
    (void)arg;
    static mn_evt_t evt;
    for (;;) {
        if (xQueueReceive(s_evt_q, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt.kind == MN_EVT_RX) {
            handle_rx(&evt);
        } else if (evt.kind == MN_EVT_ROLE) {
            emit_class(MN_EC_ROLE, "!ROLE %s", evt.role);
            if (!strcmp(evt.role, "leader") || !strcmp(evt.role, "router") ||
                !strcmp(evt.role, "child")) {
                if (s_state != MN_READY) {
                    mn_set_state(MN_READY);
                    announce_schedule();      /* jittered announce (§11.6) */
                    queue_drain();            /* replay DEGRADED-queued sends */
                    /* A node that just booted or rejoined has no clock of its
                     * own; ask rather than wait out the stratum-0 refresh.
                     * Nodes that already have one stay quiet, so a partition
                     * heal does not turn into a request storm. */
                    if (!s_clock_set) {
                        mn_time_request();
                        /* H7: an ex-anchor with no clock is the ratchet's
                         * root cause — say so instead of degrading quietly. */
                        if (s_was_anchor)
                            emit_class(MN_EC_WARN,
                                "!WARN time-anchor-await-seed — this node was "
                                "the mesh time anchor; re-seed with TIME SET "
                                "(tools/hcp.py synctime)");
                    }
                    else if (s_time_stratum == 0) mn_time_push();
                }
            } else if (!strcmp(evt.role, "detached")) {
                mn_set_state(s_state == MN_READY ? MN_DEGRADED : MN_ATTACHING);
            }
        } else if (evt.kind == MN_EVT_ANNOUNCE) {
            if (s_state == MN_READY) announce_name();
        } else if (evt.kind == MN_EVT_TIMEPUSH) {
            /* Either a jittered answer to a request that nobody else beat us
             * to, or the stratum-0 refresh. Both are just "announce now". */
            if (s_state == MN_READY) mn_time_push();
        } else if (evt.kind == MN_EVT_XFER_TICK) {
            mn_xfer_tick();               /* E-H: paced send + timeout FSMs */
        } else if (evt.kind == MN_EVT_NOTE) {
            mn_write_line((const char *)evt.data);
        } else if (evt.kind == MN_EVT_HEARTBEAT) {
            /* §4.6: heartbeat only once READY/DEGRADED */
            if (s_state == MN_READY || s_state == MN_DEGRADED) {
                emit_class(MN_EC_HEARTBEAT, "!HEARTBEAT %s %llu %s %d",
                           mn_state_name(s_state),
                           (unsigned long long)(esp_timer_get_time() / 1000000),
                           mn_ot_role_name(), peer_count());
            }
        }
    }
}

/* ---- heartbeat timer (§4.6: default 30 s, 0 = off) ---- */
static TimerHandle_t s_hb_timer = NULL;

static void hb_timer_cb(TimerHandle_t t) {
    (void)t;
    if (!s_evt_q) return;
    static mn_evt_t evt;                 /* timer-daemon task is the only poster */
    evt.kind = MN_EVT_HEARTBEAT;
    xQueueSend(s_evt_q, &evt, 0);
}

void mn_heartbeat_set(uint32_t secs) {
    if (!s_hb_timer) return;
    if (secs == 0) {
        xTimerStop(s_hb_timer, portMAX_DELAY);
        mn_emit_event("# heartbeat off");
    } else {
        xTimerChangePeriod(s_hb_timer, pdMS_TO_TICKS(secs * 1000), portMAX_DELAY);
        xTimerStart(s_hb_timer, portMAX_DELAY);
        mn_emit_event("# heartbeat every %lus", (unsigned long)secs);
    }
}

/* ---- E-G jittered announce (§11.6 Trickle-style suppression) ----
 * On a partition heal every node enters READY within the same MLE beat; an
 * immediate announce from each is a synchronized burst that scales with node
 * count. Waiting a random 200–1700 ms spreads the burst across ~10 slots. */
static TimerHandle_t s_announce_timer = NULL;

static void announce_timer_cb(TimerHandle_t t) {
    (void)t;
    if (!s_evt_q) return;
    static mn_evt_t evt;                 /* timer-daemon task is the only poster */
    evt.kind = MN_EVT_ANNOUNCE;
    xQueueSend(s_evt_q, &evt, 0);
}

static void announce_schedule(void) {
    if (!s_announce_timer) { announce_name(); return; }
    uint32_t ms = 200 + (esp_random() % 1500);
    /* one-shot: ChangePeriod also (re)starts it, collapsing rapid role flaps
     * into a single announce */
    xTimerChangePeriod(s_announce_timer, pdMS_TO_TICKS(ms), 0);
}

/* ---- E-G recent-frames ring (SED catch-up, §11.5) ----
 * Raw wire frames (header + ciphertext + MIC) so serving them is a memcpy and
 * reveals nothing beyond what already went over the air. Multicast chat only:
 * DMs are never stored — a poll must not leak someone else's unicast. */
#define MN_RECENT_MAX 12

static struct { uint16_t len; uint8_t frame[MN_ENV_MAX_FRAME]; } s_recent[MN_RECENT_MAX];
static int s_recent_head = 0;            /* next write slot */
static SemaphoreHandle_t s_recent_mutex = NULL;   /* pump task vs. senders vs. OT GET */

void mn_recent_store(const uint8_t *frame, size_t len) {
    if (!s_recent_mutex || len == 0 || len > MN_ENV_MAX_FRAME) return;
    xSemaphoreTake(s_recent_mutex, portMAX_DELAY);
    s_recent[s_recent_head].len = (uint16_t)len;
    memcpy(s_recent[s_recent_head].frame, frame, len);
    s_recent_head = (s_recent_head + 1) % MN_RECENT_MAX;
    xSemaphoreGive(s_recent_mutex);
}

int mn_recent_fill(uint8_t *buf, size_t cap) {
    if (!s_recent_mutex) return 0;
    xSemaphoreTake(s_recent_mutex, portMAX_DELAY);
    /* pick newest-first until the response is full… */
    int pick[MN_RECENT_MAX], np = 0;
    size_t need = 0;
    for (int i = 1; i <= MN_RECENT_MAX; i++) {
        int idx = (s_recent_head - i + MN_RECENT_MAX) % MN_RECENT_MAX;
        uint16_t flen = s_recent[idx].len;
        if (!flen) break;                /* ring not yet wrapped: older = empty */
        if (need + 2 + flen > cap) break;
        need += 2 + flen;
        pick[np++] = idx;
    }
    /* …then emit oldest-first so the poller's high-water dedup admits them all */
    size_t n = 0;
    for (int i = np - 1; i >= 0; i--) {
        uint16_t flen = s_recent[pick[i]].len;
        buf[n]     = (uint8_t)(flen >> 8);
        buf[n + 1] = (uint8_t)flen;
        memcpy(buf + n + 2, s_recent[pick[i]].frame, flen);
        n += 2 + flen;
    }
    xSemaphoreGive(s_recent_mutex);
    return (int)n;
}

/* Local replay for a reconnecting host (see magnet.h). Statics because both
 * the serial dispatcher and the BLE worker can run verbs — guarded by their
 * own mutex. Each frame is snapshotted under the ring mutex then processed
 * outside it: emitting takes the TX mutex, and the send path already nests
 * TX-mutex → ring-mutex, so nesting the other way here would ABBA. */
void mn_recent_print(void) {
    static SemaphoreHandle_t print_mutex = NULL;
    static uint8_t frame[MN_ENV_MAX_FRAME], pt[MN_ENV_MAX_FRAME];
    static char text[MN_ENV_MAX_FRAME];
    if (!s_recent_mutex) { mn_emit_event("# recent local: 0 frame(s)"); return; }
    if (!print_mutex) print_mutex = xSemaphoreCreateMutex();  /* first call is
        long after boot; worst case two callers race once and leak one mutex */
    xSemaphoreTake(print_mutex, portMAX_DELAY);

    int emitted = 0;
    for (int i = MN_RECENT_MAX; i >= 1; i--) {               /* oldest first */
        uint16_t flen = 0;
        xSemaphoreTake(s_recent_mutex, portMAX_DELAY);
        int idx = (s_recent_head - i + MN_RECENT_MAX) % MN_RECENT_MAX;
        if (s_recent[idx].len) {
            flen = s_recent[idx].len;
            memcpy(frame, s_recent[idx].frame, flen);
        }
        xSemaphoreGive(s_recent_mutex);
        if (!flen) continue;

        mn_envelope_t env;
        if (mn_env_unpack(&env, frame, flen) != 0) continue;
        if (env.selector != s_chan.selector) continue;       /* stale leftovers */
        if (env.type != MN_T_CHAT) continue;                 /* ring is chat-only */
        if (env.flags & MN_F_ENCRYPTED) {
            if (env.payload_len <= 8) continue;
            size_t ct_len = env.payload_len - 8;
            uint8_t nonce[13];
            mn_nonce_build(nonce, env.sender_id, env.counter, env.epoch, env.selector);
            const uint8_t *key = (env.epoch == s_epoch) ? s_chan.epoch_key :
                                 (env.epoch == (uint8_t)(s_epoch - 1)) ? s_prev_epoch_key : NULL;
            if (!key || mn_aead_decrypt(key, nonce, frame, env.payload, ct_len, pt,
                                        env.payload + ct_len) != 0) continue;
            env.payload = pt;
            env.payload_len = ct_len;
        }

        char idhex[9];
        snprintf(idhex, sizeof(idhex), "%02x%02x%02x%02x",
                 env.sender_id[0], env.sender_id[1], env.sender_id[2], env.sender_id[3]);
        const char *nm = !memcmp(env.sender_id, s_device_id, 4)
                             ? s_name : peer_name(env.sender_id);
        size_t n = env.payload_len < sizeof(text) - 1 ? env.payload_len : sizeof(text) - 1;
        memcpy(text, env.payload, n);
        text[n] = '\0';
        emit_class(MN_EC_CHAT, "!RCHAT %s %s %s %s", MN_CHANNEL_NAME, idhex, nm, text);
        emitted++;
    }
    mn_emit_event("# recent local: %d frame(s)", emitted);
    xSemaphoreGive(print_mutex);
}

/* =========================== bringup ====================================== */
void mn_core_init(void) {
    if (!s_tx_mutex) s_tx_mutex = xSemaphoreCreateRecursiveMutex();
    s_state = MN_BOOTING;
    memset(s_peers, 0, sizeof(s_peers));
    memset(s_dedup, 0, sizeof(s_dedup));
    name_load();                         /* NVS must be up (main inits it first) */
    counter_load();                      /* §11.1.6 block-reserved nonce counter */
    chan_load();                         /* active channel (default: "magnet")  */
    anchor_load();                       /* H7: were we the time anchor?        */
    if (mn_ident_load_or_gen(s_pub, s_device_id) != 0)
        mn_emit_event("!WARN identity-keygen-failed");
    admins_load();
    script_load();
    if (!strcmp(s_chan.name, "magnet"))
        mn_emit_event("!WARN default-channel-insecure use CHANNEL SET");
    if (!s_evt_q) {
        /* E-G: 12 deep — a catch-up response replays up to 12 frames back-to-
         * back from the OT task; an 8-deep queue dropped the tail */
        s_evt_q = xQueueCreate(12, sizeof(mn_evt_t));
        xTaskCreate(pump_task, "mn_pump", 4096, NULL, 5, NULL);
    }
    if (!s_selftest_sem) s_selftest_sem = xSemaphoreCreateBinary();
    if (!s_forth_mutex)  s_forth_mutex  = xSemaphoreCreateMutex();
    if (!s_recent_mutex) s_recent_mutex = xSemaphoreCreateMutex();
    mn_xfer_init();                      /* E-H transfer mutex + tick timer */
    if (!s_hb_timer) {
        s_hb_timer = xTimerCreate("mn_hb", pdMS_TO_TICKS(30 * 1000), pdTRUE,
                                  NULL, hb_timer_cb);
        xTimerStart(s_hb_timer, 0);
    }
    if (!s_announce_timer) {
        s_announce_timer = xTimerCreate("mn_ann", pdMS_TO_TICKS(1000), pdFALSE,
                                        NULL, announce_timer_cb);
    }
    /* One one-shot timer serves both time jobs: the jittered reply to a
     * request, and the stratum-0 refresh. They never overlap — a node is
     * either answering right now or waiting out its long refresh — so one
     * handle is enough and the reload period is set at each use site. */
    if (!s_time_timer) {
        s_time_timer = xTimerCreate("mn_time", pdMS_TO_TICKS(1000), pdFALSE,
                                    NULL, time_timer_cb);
    }
}

/* ====================== core ops (HCP + Forth share these) ================= */

/* §11.1.6 persistent counter, block-reserved to avoid a flash write per
 * message: NVS always holds a value ≥ any counter ever used. Fail closed. */
#define MN_CNT_BLOCK 1024
static uint32_t s_counter_limit = 0;

static int counter_reserve(void) {
    nvs_handle_t h;
    if (nvs_open("magnet", NVS_READWRITE, &h) != ESP_OK) return -1;
    int rc = (nvs_set_u32(h, "cnt", s_counter + MN_CNT_BLOCK) == ESP_OK &&
              nvs_commit(h) == ESP_OK) ? 0 : -1;
    if (rc == 0) s_counter_limit = s_counter + MN_CNT_BLOCK;
    nvs_close(h);
    return rc;
}

static void counter_load(void) {
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("magnet", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "cnt", &v);
        nvs_close(h);
    }
    s_counter = v;               /* stored value is ≥ last used + 1 (block gap) */
    counter_reserve();
}

static int send_frame_ex(uint8_t type, const uint8_t *payload, size_t len,
                         const char *dst, bool con, bool admin,
                         uint8_t extra_flags) {
    if (!payload || len == 0) return -1;
    if (len > MN_ENV_MAX_FRAME - MN_ENV_HDR_LEN - 8 - 64) return -2;
    if (s_counter + 1 >= s_counter_limit && counter_reserve() != 0)
        return -5;               /* MUST NOT transmit without a fresh nonce */

    mn_envelope_t env = {
        .version    = MN_ENV_VERSION,
        .type       = type,
        .flags      = (uint8_t)((s_chan.set ? MN_F_ENCRYPTED : 0) |
                                (admin ? (MN_F_ADMIN | MN_F_SIGNED) : 0) |
                                extra_flags),
        .counter    = ++s_counter,
        .epoch      = s_epoch,
        .selector   = s_chan.selector,
        .frag_total = 1,
        .frag_idx   = 0,
        .msg_id     = (uint16_t)esp_random(),
    };
    memcpy(env.sender_id, s_device_id, 4);

    uint8_t frame[MN_ENV_MAX_FRAME];
    int n = mn_env_pack(frame, sizeof(frame), &env, payload, len);
    if (n < 0) return n;

    if (s_chan.set) {            /* encrypt payload in place, append MIC */
        uint8_t nonce[13];
        mn_nonce_build(nonce, env.sender_id, env.counter, env.epoch, env.selector);
        uint8_t ct[MN_ENV_MAX_FRAME];
        if (mn_aead_encrypt(s_chan.epoch_key, nonce, frame,
                            frame + MN_ENV_HDR_LEN, len, ct,
                            frame + MN_ENV_HDR_LEN + len) != 0) return -6;
        memcpy(frame + MN_ENV_HDR_LEN, ct, len);
        n += 8;
    }
    if (admin) {                 /* §11.1.7: sig over header‖ciphertext‖MIC */
        if (mn_sign(frame, (size_t)n, frame + n) != 0) return -7;
        n += 64;
    }

    s_stats.tx_try++;
    int rc = mn_ot_send(frame, (size_t)n, dst, con);
    if (rc == 0) {
        s_stats.tx_ok++;
        s_stats.tx_bytes += len;
        /* E-G: our own multicast chat joins the catch-up ring too, so a
         * poller gets the full channel history, not just what we overheard.
         * Stress frames stay out — they'd flush 12 real messages in ~10 ms. */
        if (type == MN_T_CHAT && !dst &&
            !(len >= sizeof(MN_STRESS_MARK) &&
              !memcmp(payload, MN_STRESS_MARK, sizeof(MN_STRESS_MARK))))
            mn_recent_store(frame, (size_t)n);
    } else {
        s_stats.tx_err++;
    }
    return rc;
}

/* ---- channel management (derive/persist/switch) ---- */
static void chan_persist(void) {
    nvs_handle_t h;
    if (nvs_open("magnet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "chroot", s_chan.root, 32);
    nvs_set_str(h, "chname", s_chan.name);
    nvs_set_u8(h, "chpath", (uint8_t)s_chan.cred_path);
    nvs_commit(h);
    nvs_close(h);
}

static void chan_load(void) {
    nvs_handle_t h;
    size_t blen = 32, nlen = sizeof(s_chan.name);
    uint8_t path = 0;
    if (nvs_open("magnet", NVS_READONLY, &h) == ESP_OK &&
        nvs_get_blob(h, "chroot", s_chan.root, &blen) == ESP_OK && blen == 32) {
        nvs_get_str(h, "chname", s_chan.name, &nlen);
        nvs_get_u8(h, "chpath", &path);
        s_chan.cred_path = (char)(path ? path : 'B');
        mn_root_expand(&s_chan);
        s_chan.set = true;
        nvs_close(h);
        return;
    }
    /* first boot: derive the well-known default (§11.5) and cache it */
    mn_cred_derive("magnet", 6, &s_chan);
    chan_persist();
}

int mn_channel_set(const char *cred, size_t len, char *info, size_t cap) {
    mn_channel_t nc;
    int rc = mn_cred_derive(cred, len, &nc);
    if (rc != 0) return rc;
    s_chan = nc;
    s_epoch = 0;                                 /* fresh channel → epoch 0 */
    memset(s_prev_epoch_key, 0, sizeof(s_prev_epoch_key));
    chan_persist();
    mn_ot_set_mcast(s_chan.mcast_suffix);        /* no-op if radio not up */
    memset(s_dedup, 0, sizeof(s_dedup));         /* new channel, new peers */
    memset(s_peers, 0, sizeof(s_peers));
    if (s_recent_mutex) {                        /* stale-channel frames out */
        xSemaphoreTake(s_recent_mutex, portMAX_DELAY);
        memset(s_recent, 0, sizeof(s_recent));
        s_recent_head = 0;
        xSemaphoreGive(s_recent_mutex);
    }
    if (info) snprintf(info, cap, "%s selector=%04x path=%c",
                       s_chan.name, s_chan.selector, s_chan.cred_path);
    /* §12.9 Q3: provisioning done → reclaim the radio and NimBLE's RAM.
     * Companion builds (MN_BLE_RESIDENT) keep the stack up — the bonded link
     * IS the app's window into the mesh. */
#if !MN_BLE_RESIDENT
    if (mn_ble_running()) mn_ble_stop();
#endif
    if (s_state == MN_READY) announce_name();
    return 0;
}

void mn_channel_info(char *buf, size_t cap) {
    snprintf(buf, cap, "name=%s selector=%04x mcast=ff05::%02x%02x:%02x%02x path=%c",
             s_chan.name, s_chan.selector,
             s_chan.mcast_suffix[0], s_chan.mcast_suffix[1],
             s_chan.mcast_suffix[2], s_chan.mcast_suffix[3], s_chan.cred_path);
}

const uint8_t *mn_channel_mcast_suffix(void) { return s_chan.mcast_suffix; }

bool mn_channel_is_default(void) { return strcmp(s_chan.name, "magnet") == 0; }

static int send_frame(uint8_t type, const uint8_t *payload, size_t len,
                      const char *dst, bool con) {
    return send_frame_ex(type, payload, len, dst, con, false, 0);
}

/* E-H: Type 6 frames ride the normal envelope/AEAD/counter path — CON
 * unicast only (docs/EXTENDED-TRANSFER.md), never the multicast group. */
int mn_xfer_send_frame(const uint8_t *payload, size_t len, const char *dst_ipv6) {
    if (!dst_ipv6) return -2;
    return send_frame(MN_T_XFER6, payload, len, dst_ipv6, true);
}

bool mn_xfer_ec_enabled(void) { return (s_ev_mask & MN_EC_XFER) != 0; }

/* Token bucket for host-initiated multicast (§4.9): burst 8, refill 10/s.
 * STRESS bypasses it (calls send_frame directly — it measures the stack). */
static uint32_t s_bucket = 8;
static int64_t  s_bucket_us = 0;

static bool rate_ok(void) {
    int64_t now = esp_timer_get_time();
    if (s_bucket_us == 0) s_bucket_us = now;
    uint32_t refill = (uint32_t)((now - s_bucket_us) / 100000);  /* 1 per 100 ms */
    if (refill) {
        s_bucket = s_bucket + refill > 8 ? 8 : s_bucket + refill;
        s_bucket_us += (int64_t)refill * 100000;
    }
    if (s_bucket == 0) return false;
    s_bucket--;
    return true;
}

int mn_chat(const char *msg, size_t len) {
    if (!rate_ok()) return -4;                  /* -ERR E_RATE_LIMITED */
    return send_frame(MN_T_CHAT, (const uint8_t *)msg, len, NULL, false);
}

int mn_dm(const char *peer_ipv6, const char *msg, size_t len) {
    return send_frame(MN_T_CHAT, (const uint8_t *)msg, len, peer_ipv6, true);
}

/* Bot replies go out marked MN_F_AUTOMATED so no other auto-responder answers
 * them. They still pay the normal §4.9 rate limit — a bot is a host like any
 * other, and letting it bypass the bucket would defeat the point. */
int mn_chat_automated(const char *msg, size_t len) {
    if (!rate_ok()) return -4;
    return send_frame_ex(MN_T_CHAT, (const uint8_t *)msg, len, NULL, false,
                         false, MN_F_AUTOMATED);
}

/* ---- host-set wall clock (§4.6 addendum) ----
 * A Hanasu mesh has no border router and therefore no SNTP: esp_timer only
 * ever gives uptime. Any host on the HCP link (the phone app, hcp.py, the
 * soak harness) does know the time, so it can push it down once per session.
 * Unset is the normal state and must stay useful — mn_time_str() falls back
 * to uptime rather than lying about a date.
 *
 * Deliberately NOT settimeofday + strftime + tzset: that path costs ~8.8 KB
 * of flash in newlib's time formatting and TZ parsing, measured, to print
 * eight characters. The clock is anchored to esp_timer instead and the time
 * of day comes out of three integer divisions. The trade is that only the
 * time of day is available (no date, no DST rules) — which is all a bot line
 * or a log stamp on a bench node ever wanted. */
/* State and constants live near the top of the file with the rest; only the
 * behaviour is here. See "host-set wall clock" there.
 *
 * ---- mesh-wide clock distribution (system/time, ns 0x00 cmd 0x04/0x05) ----
 * One host seeds ONE node over HCP; that node becomes stratum 0 and multicasts
 * the clock to the channel. Everyone else adopts it at stratum+1. The stratum
 * is what keeps a mesh with two seeded nodes from oscillating: a better source
 * always wins, an equal source only wins on a deterministic tie-break, and a
 * worse one is ignored outright.
 *
 * Nodes do NOT re-broadcast what they hear. Thread's MPL already floods
 * realm-local multicast across the whole mesh, so a re-broadcast would buy no
 * reach and cost a storm. Convergence for late joiners and rebooted nodes
 * comes from two things instead: the stratum-0 node refreshes periodically,
 * and any node can pull with a request (cmd 0x05).
 *
 * Trust model: these frames are channel-encrypted like everything else, so
 * anyone who can send chat can set the mesh clock. That is deliberate — it is
 * the same boundary chat already has, and NOTHING security-critical depends on
 * the clock (the replay defence is the monotonic counter in §11.1, not a
 * timestamp). If that ever changes, this becomes an ADMIN|SIGNED frame like
 * system/rotate, which is why the payload carries a stratum and not a bare
 * timestamp. */

/* Apply a clock from any source. stratum 0 = seeded here by a host. */
static int time_apply(int64_t epoch_secs, int tz_offset_min, uint8_t stratum,
                      const uint8_t src[4]) {
    if (epoch_secs < 1700000000LL) return -1;      /* sanity: after 2023-11 */
    if (tz_offset_min < -720 || tz_offset_min > 840) return -1;
    s_epoch_base = epoch_secs;
    s_uptime_at_set_us = esp_timer_get_time();
    s_tz_offset_min = tz_offset_min;
    s_time_stratum = stratum;
    s_time_learned_us = s_uptime_at_set_us;
    memcpy(s_time_src, src, 4);
    s_clock_set = true;

    /* Receivers never re-broadcast, so the ONLY way to land above stratum 1 is
     * to have adopted from a node that had itself adopted — i.e. no stratum-0
     * anchor answered. That is precisely the "the host-seeded node rebooted
     * and its RAM-only clock went with it" case, and it RATCHETS: every
     * subsequent reboot adopts from a neighbour one hop worse, until the mesh
     * hits MN_TIME_MAX_STRATUM and stops distributing time at all. It is
     * self-healing while an anchor lives (a stratum-0 refresh always wins) and
     * unrecoverable once one does not, so say so loudly rather than degrade in
     * silence. Fix is operational: re-seed any node with TIME SET. */
    if (stratum >= 2)
        emit_class(MN_EC_WARN,
                   "!WARN time-no-anchor stratum=%u — no stratum-0 node on this "
                   "mesh; re-seed with TIME SET (tools/hcp.py synctime)", stratum);
    return 0;
}

int mn_time_set(int64_t epoch_secs, int tz_offset_min) {
    int rc = time_apply(epoch_secs, tz_offset_min, 0, s_device_id);
    if (rc != 0) return rc;
    /* H7: a host seed makes this node THE anchor — remember that across
     * reboot (the role, never the clock). */
    if (!s_was_anchor) anchor_persist(true);
    s_was_anchor = true;
    s_anchor_warned = false;
    mn_time_push();          /* a host seed is news — tell the mesh at once */
    return 0;
}

bool mn_time_is_set(void) { return s_clock_set; }

void mn_time_info(char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (!s_clock_set) { snprintf(buf, cap, "clock=unset"); return; }
    char when[32];
    mn_time_str(when, sizeof(when));
    unsigned age = (unsigned)((esp_timer_get_time() - s_time_learned_us) / 1000000);
    if (s_time_stratum == 0) {
        snprintf(buf, cap, "clock=%s tz=%+d stratum=0 src=host age=%us",
                 when, s_tz_offset_min, age);
    } else {
        snprintf(buf, cap,
                 "clock=%s tz=%+d stratum=%u src=%02x%02x%02x%02x age=%us",
                 when, s_tz_offset_min, s_time_stratum, s_time_src[0],
                 s_time_src[1], s_time_src[2], s_time_src[3], age);
    }
}

void mn_time_str(char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (s_clock_set) {
        int64_t now = s_epoch_base + s_tz_offset_min * 60 +
                      (esp_timer_get_time() - s_uptime_at_set_us) / 1000000;
        int32_t sod = (int32_t)(((now % 86400) + 86400) % 86400);
        snprintf(buf, cap, "%02d:%02d:%02d", (int)(sod / 3600),
                 (int)((sod % 3600) / 60), (int)(sod % 60));
        return;
    }
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(buf, cap, "up %luh%02lum", (unsigned long)(up / 3600),
             (unsigned long)((up % 3600) / 60));
}

/* Current UTC epoch as this node believes it, or 0 when the clock is unset. */
static int64_t time_now_epoch(void) {
    if (!s_clock_set) return 0;
    return s_epoch_base + (esp_timer_get_time() - s_uptime_at_set_us) / 1000000;
}

/* system/time announce: ns 0x00, cmd 0x04, 11 params —
 *   [0..7] epoch seconds, int64 BE   [8..9] tz offset minutes, int16 BE
 *   [10]   stratum of the SENDER (receivers adopt stratum+1)                */
int mn_time_push(void) {
    if (!s_clock_set) return -1;
    int64_t now = time_now_epoch();
    uint8_t pl[4 + 11];
    pl[0] = 0x00; pl[1] = 0x04; pl[2] = 0; pl[3] = 11;
    for (int i = 0; i < 8; i++) pl[4 + i] = (uint8_t)(now >> (56 - 8 * i));
    pl[12] = (uint8_t)((s_tz_offset_min >> 8) & 0xff);
    pl[13] = (uint8_t)(s_tz_offset_min & 0xff);
    pl[14] = s_time_stratum;
    int rc = send_frame(MN_T_M2M_CMD, pl, sizeof(pl), NULL, false);
    /* Stratum 0 is the mesh's anchor: keep re-announcing so late joiners and
     * rebooted nodes converge without anyone having to ask. */
    if (s_time_timer && s_time_stratum == 0)
        xTimerChangePeriod(s_time_timer, pdMS_TO_TICKS(MN_TIME_REFRESH_MS), 0);
    return rc;
}

/* system/time request: ns 0x00, cmd 0x05, no params. */
int mn_time_request(void) {
    uint8_t pl[4] = { 0x00, 0x05, 0x00, 0x00 };
    return send_frame(MN_T_M2M_CMD, pl, sizeof(pl), NULL, false);
}

/* Schedule a jittered answer to a request (§11.6 Trickle-style suppression).
 * The delay is biased by stratum so the best clock in earshot answers first
 * and everyone else hears it and stands down — one reply, whatever the mesh
 * size. Nodes with no clock never schedule. */
static void time_reply_schedule(void) {
    if (!s_clock_set || !s_time_timer) return;
    uint32_t base = (uint32_t)s_time_stratum * 400;      /* s0: 0-, s1: 400-… */
    uint32_t ms = base + 50 + (esp_random() % 350);
    xTimerChangePeriod(s_time_timer, pdMS_TO_TICKS(ms), 0);
}

static void time_timer_cb(TimerHandle_t t) {
    (void)t;
    if (!s_evt_q) return;
    static mn_evt_t evt;                 /* timer-daemon task is the only poster */
    evt.kind = MN_EVT_TIMEPUSH;
    xQueueSend(s_evt_q, &evt, 0);
}

/* system/announce: Type 1, ns 0x00, cmd 0x02, params = display name */
static void announce_name(void) {
    if (s_name[0] == '\0' || !strcmp(s_name, "-")) return;
    uint8_t pl[4 + MN_NAME_MAX];
    size_t n = strlen(s_name);
    pl[0] = 0x00; pl[1] = 0x02;
    pl[2] = 0; pl[3] = (uint8_t)n;
    memcpy(&pl[4], s_name, n);
    send_frame(MN_T_M2M_CMD, pl, 4 + n, NULL, false);
}

/* ---- DEGRADED command queue (§4.6: max 4, replay on READY, !RESULT) ---- */
#define MN_QUEUE_MAX 4
static struct {
    bool used, is_dm;
    char dst[46];
    uint16_t len;
    uint8_t text[MN_ENV_MAX_FRAME - MN_ENV_HDR_LEN];
} s_queue[MN_QUEUE_MAX];

int mn_queue_chat(const char *dst, const char *msg, size_t len) {
    if (len > sizeof(s_queue[0].text)) return -1;
    for (int i = 0; i < MN_QUEUE_MAX; i++) {
        if (s_queue[i].used) continue;
        s_queue[i].used = true;
        s_queue[i].is_dm = (dst != NULL);
        if (dst) strlcpy(s_queue[i].dst, dst, sizeof(s_queue[i].dst));
        memcpy(s_queue[i].text, msg, len);
        s_queue[i].len = (uint16_t)len;
        int depth = 0;
        for (int j = 0; j < MN_QUEUE_MAX; j++) if (s_queue[j].used) depth++;
        return depth;
    }
    return -1;                                  /* full → E_QUEUE_FULL */
}

static void queue_drain(void) {                 /* pump task, on entering READY */
    for (int i = 0; i < MN_QUEUE_MAX; i++) {
        if (!s_queue[i].used) continue;
        int rc = send_frame(MN_T_CHAT, s_queue[i].text, s_queue[i].len,
                            s_queue[i].is_dm ? s_queue[i].dst : NULL,
                            s_queue[i].is_dm);
        s_queue[i].used = false;
        mn_emit_event("!RESULT @- %s %s", rc == 0 ? "ok" : "err",
                      s_queue[i].is_dm ? "dm" : "chat");
    }
}

/* ---- E-E: serialized Forth execution + automation hooks ---- */
void mn_forth_exec(const char *line) {
    /* Lock order everywhere: TX mutex (output atomicity) then engine mutex. */
    SemaphoreHandle_t tx = s_tx_mutex;
    if (tx) xSemaphoreTakeRecursive(tx, portMAX_DELAY);
    if (s_forth_mutex) xSemaphoreTake(s_forth_mutex, portMAX_DELAY);
    forth_eval(line);
    if (s_forth_mutex) xSemaphoreGive(s_forth_mutex);
    if (tx) xSemaphoreGiveRecursive(tx);
}

int mn_hook_set(int kind, const char *word, size_t len) {
    if (len > MN_HOOK_WORD_MAX) return -1;
    char *dst = (kind == 0) ? s_hook_chat : s_hook_cmd;
    if (len == 0) { dst[0] = '\0'; return 0; }          /* empty = unregister */
    memcpy(dst, word, len);
    dst[len] = '\0';
    return 0;
}

void mn_hooks_print(void) {
    mn_emit_event("# hook chat=%s cmd=%s",
                  s_hook_chat[0] ? s_hook_chat : "-",
                  s_hook_cmd[0]  ? s_hook_cmd  : "-");
}

/* Invoked from the PUMP TASK only (never OT context). The message is pushed
 * as ( c-addr u ) so the user word can inspect it with str= / type. */
static char s_hook_arg[MN_ENV_MAX_FRAME];

/* Amplification guard (found by the E-E bench test): a hook that sends chat
 * will re-trigger the *other* nodes' hooks, whose replies re-trigger ours —
 * a self-sustaining mesh loop. Cap hook firings at 5/s; beyond that, suppress
 * and warn once until the storm subsides. Cheap, bounded, and it keeps a
 * badly-written user script from taking the channel down. */
#define MN_HOOK_MAX_PER_SEC 5
static int     s_hook_budget = MN_HOOK_MAX_PER_SEC;
static int64_t s_hook_window_us = 0;
static bool    s_hook_warned = false;

static bool hook_rate_ok(void) {
    int64_t now = esp_timer_get_time();
    if (now - s_hook_window_us >= 1000000) {
        s_hook_window_us = now;
        s_hook_budget = MN_HOOK_MAX_PER_SEC;
        s_hook_warned = false;
    }
    if (s_hook_budget <= 0) {
        if (!s_hook_warned) {
            s_hook_warned = true;
            emit_class(MN_EC_WARN, "!WARN hook-rate-limited (loop?) suppressing");
        }
        return false;
    }
    s_hook_budget--;
    return true;
}

static void hook_invoke(const char *word, const uint8_t *payload, size_t len) {
    if (!word || !word[0]) return;
    if (!hook_rate_ok()) return;
    size_t n = len < sizeof(s_hook_arg) - 1 ? len : sizeof(s_hook_arg) - 1;
    memcpy(s_hook_arg, payload, n);
    s_hook_arg[n] = '\0';
    forth_push((intptr_t)s_hook_arg);
    forth_push((intptr_t)n);
    mn_forth_exec(word);
}

/* ---- boot script persistence (NVS blob, run after vocab registration) ---- */
#define MN_SCRIPT_MAX 1024
static char s_script[MN_SCRIPT_MAX + 1];

int mn_script_save(const char *src, size_t len) {
    if (len > MN_SCRIPT_MAX) return -1;
    memcpy(s_script, src, len);
    s_script[len] = '\0';
    nvs_handle_t h;
    if (nvs_open("magnet", NVS_READWRITE, &h) != ESP_OK) return -2;
    int rc = (nvs_set_str(h, "script", s_script) == ESP_OK &&
              nvs_commit(h) == ESP_OK) ? 0 : -2;
    nvs_close(h);
    return rc;
}

static void script_load(void) {
    nvs_handle_t h;
    size_t len = sizeof(s_script);
    if (nvs_open("magnet", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "script", s_script, &len) != ESP_OK) s_script[0] = '\0';
        nvs_close(h);
    }
}

void mn_script_show(void) {
    if (!s_script[0]) { mn_emit_event("# script: (empty)"); return; }
    /* one # line per source line so HCP framing holds */
    const char *p = s_script;
    while (*p) {
        char line[128];
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;
        mn_emit_event("# script| %s", line);
    }
}

int mn_script_run(void) {
    if (!s_script[0]) return -1;
    const char *p = s_script;
    while (*p) {                       /* the engine evaluates one line at a time */
        char line[160];
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;
        if (line[0]) mn_forth_exec(line);
    }
    return 0;
}

/* ---- admin allow-list (NVS blob: N x 65-byte pubkeys) ---- */
static void admins_persist(void) {
    uint8_t blob[MN_ADMIN_MAX * 65];
    size_t n = 0;
    for (int i = 0; i < MN_ADMIN_MAX; i++)
        if (s_admins[i].used) { memcpy(blob + n, s_admins[i].pub, 65); n += 65; }
    nvs_handle_t h;
    if (nvs_open("magnet", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "admins", blob, n);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void admins_load(void) {
    uint8_t blob[MN_ADMIN_MAX * 65];
    size_t n = sizeof(blob);
    nvs_handle_t h;
    if (nvs_open("magnet", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "admins", blob, &n) == ESP_OK) {
        for (size_t i = 0; i + 65 <= n && i / 65 < MN_ADMIN_MAX; i += 65) {
            s_admins[i / 65].used = true;
            memcpy(s_admins[i / 65].pub, blob + i, 65);
        }
    }
    nvs_close(h);
}

int mn_admin_add(const uint8_t pub65[65]) {
    for (int i = 0; i < MN_ADMIN_MAX; i++)
        if (s_admins[i].used && !memcmp(s_admins[i].pub, pub65, 65)) return 0;
    for (int i = 0; i < MN_ADMIN_MAX; i++) {
        if (s_admins[i].used) continue;
        s_admins[i].used = true;
        memcpy(s_admins[i].pub, pub65, 65);
        admins_persist();
        return 0;
    }
    return -1;
}

void mn_admin_list_print(void) {
    int n = 0;
    for (int i = 0; i < MN_ADMIN_MAX; i++) {
        if (!s_admins[i].used) continue;
        char hex[24];
        for (int j = 0; j < 8; j++) sprintf(hex + j * 2, "%02x", s_admins[i].pub[j]);
        mn_emit_event("# admin key %d: %s... (65B)", ++n, hex);
    }
    if (!n) mn_emit_event("# admin allow-list empty");
}

const uint8_t *mn_pubkey(void) { return s_pub; }

/* ---- factory reset (§11.3.4) ----
 * Erase every byte this node has been told, so it comes back indistinguishable
 * from one fresh off the reel: channel credential, name, admin allow-list,
 * autorun script, replay counter block, and the device identity key.
 *
 * Deliberately erase_all on the namespace rather than naming each key. A
 * key-by-key list is a maintenance trap — add a setting later, forget to add
 * it here, and "factory reset" quietly leaves state behind. That is exactly
 * the failure a reset must never have.
 *
 * The identity key goes too. A reset node is one leaving your trust domain
 * (resold, redeployed, handed on); keeping its keypair would let it carry the
 * authority some other node's allow-list still grants it. mn_ident_load_or_gen()
 * mints a fresh one on the next boot, exactly as it does on a virgin part.
 *
 * BLE bonds live in NimBLE's own namespace, so they need a second erase — a
 * node that kept its bonds would refuse to re-pair with a phone that has
 * forgotten it (mismatched LTKs), which is the one state a reset must resolve.
 * Erased unconditionally: a node reflashed from a BLE build to a non-BLE one
 * still has the old bonds sitting in flash.
 *
 * Does NOT reboot — RAM still holds the old state, so the caller must restart
 * once it has flushed its response.
 */
int mn_factory_reset(void) {
    nvs_handle_t h;
    int rc = 0;

    if (nvs_open("magnet", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_all(h) != ESP_OK || nvs_commit(h) != ESP_OK) rc = -1;
        nvs_close(h);
    } else {
        rc = -1;   /* nothing to erase is fine; unable to open is not */
    }

    if (nvs_open("nimble_bond", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);          /* absent on a non-BLE build — not an error */
        nvs_commit(h);
        nvs_close(h);
    }
    return rc;
}

static bool admin_verify(const uint8_t *signed_part, size_t len, const uint8_t sig[64]) {
    for (int i = 0; i < MN_ADMIN_MAX; i++)
        if (s_admins[i].used &&
            mn_verify(s_admins[i].pub, signed_part, len, sig) == 0) return true;
    return false;
}

static void epoch_apply(uint8_t e) {
    memcpy(s_prev_epoch_key, s_chan.epoch_key, 16);   /* skew window (§11.1.8) */
    s_epoch = e;
    mn_chan_epoch_key(&s_chan, e);
    mn_emit_event("!WARN epoch-rotated to %u", e);
}

/* signed system/rotate (ns 0, cmd 0x03): ADMIN|SIGNED multicast */
int mn_rotate(void) {
    uint8_t pl[5] = { 0x00, 0x03, 0x00, 0x01, (uint8_t)(s_epoch + 1) };
    int rc = send_frame_ex(MN_T_M2M_CMD, pl, sizeof(pl), NULL, false, true, 0);
    if (rc == 0) epoch_apply(s_epoch + 1);
    return rc;
}

void mn_status_line(char *buf, size_t cap) {
    snprintf(buf, cap, "state=%s role=%s channel=%s peers=%d name=%s id=%02x%02x%02x%02x ble=%s",
             mn_state_name(s_state), mn_ot_role_name(), MN_CHANNEL_NAME,
             peer_count(), s_name,
             s_device_id[0], s_device_id[1], s_device_id[2], s_device_id[3],
             mn_ble_running() ? "up" : "off");
}

void mn_peers_print(void) {
    int n = 0;
    uint32_t now = xTaskGetTickCount();
    for (int i = 0; i < MN_PEER_MAX; i++) {
        if (!s_peers[i].used) continue;
        mn_emit_event("# peer %02x%02x%02x%02x %s %s last-seen=%lus ago",
                      s_peers[i].id[0], s_peers[i].id[1],
                      s_peers[i].id[2], s_peers[i].id[3],
                      s_peers[i].name[0] ? s_peers[i].name : "-",
                      s_peers[i].ipv6,
                      (unsigned long)((now - s_peers[i].last_seen_ticks) / configTICK_RATE_HZ));
        n++;
    }
    if (!n) mn_emit_event("# peers: none seen yet");
}

void mn_whoami_line(char *buf, size_t cap) {
    /* E-Phase D: real device_id = SHA256(Ed25519 pubkey)[0:4]. E-B/C: EUI-64 tail. */
    snprintf(buf, cap, "id=%02x%02x%02x%02x name=%s fw=%s",
             s_device_id[0], s_device_id[1], s_device_id[2], s_device_id[3],
             s_name, MN_FW_VERSION);
}

/* ---- display name (NVS-persisted; announces on change when READY) ---- */
const char *mn_name_get(void) { return s_name; }

void mn_name_set(const char *name) {
    strlcpy(s_name, name, sizeof(s_name));
    nvs_handle_t h;
    if (nvs_open("magnet", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "name", s_name);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_state == MN_READY) announce_name();
}

static void name_load(void) {
    nvs_handle_t h;
    if (nvs_open("magnet", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_name);
        if (nvs_get_str(h, "name", s_name, &len) != ESP_OK) strlcpy(s_name, "-", sizeof(s_name));
        nvs_close(h);
    }
}

/* ======================= diagnostics / test surface ======================== */

void mn_sysinfo_print(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);

    mn_emit_event("# sys chip=%s rev=%u cores=%d idf=%s",
                  CONFIG_IDF_TARGET, chip.revision, chip.cores, esp_get_idf_version());
    mn_emit_event("# sys flash=%luKB reset-reason=%d uptime=%llus",
                  (unsigned long)(flash_sz / 1024), (int)esp_reset_reason(),
                  (unsigned long long)(esp_timer_get_time() / 1000000));
    mn_emit_event("# sys heap free=%u largest=%u min-ever=%u",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    mn_emit_event("# sys forth-heap used=%d free=%d",
                  forth_heap_used(), forth_heap_free());
}

/* Build one Type-5 PING frame carrying the selftest magic. */
static int build_selftest_ping(uint8_t *frame, size_t cap) {
    mn_envelope_t env = {
        .version    = MN_ENV_VERSION,
        .type       = MN_T_PING,
        .flags      = 0,
        .counter    = ++s_counter,
        .epoch      = 0,
        .selector   = s_chan.selector,
        .frag_total = 1,
        .frag_idx   = 0,
        .msg_id     = (uint16_t)esp_random(),
    };
    memcpy(env.sender_id, s_device_id, 4);
    return mn_env_pack(frame, cap, &env, MN_SELFTEST_MAGIC, sizeof(MN_SELFTEST_MAGIC));
}

void mn_bench(void) {
    /* 1. envelope codec: pack+unpack a 62-byte payload, N iterations */
    enum { N = 2000 };
    uint8_t payload[62], frame[MN_ENV_MAX_FRAME];
    memset(payload, 0xa5, sizeof(payload));
    mn_envelope_t env = {
        .version = MN_ENV_VERSION, .type = MN_T_CHAT, .frag_total = 1,
        .selector = s_chan.selector, .msg_id = 1,
    };
    memcpy(env.sender_id, s_device_id, 4);

    int64_t t0 = esp_timer_get_time();
    volatile int sink = 0;
    for (int i = 0; i < N; i++) {
        env.counter = i;
        sink += mn_env_pack(frame, sizeof(frame), &env, payload, sizeof(payload));
    }
    int64_t t1 = esp_timer_get_time();
    mn_envelope_t out;
    for (int i = 0; i < N; i++) sink += mn_env_unpack(&out, frame, MN_ENV_HDR_LEN + sizeof(payload));
    int64_t t2 = esp_timer_get_time();
    (void)sink;
    mn_emit_event("# bench env pack=%lldus/op unpack=%lldus/op (n=%d, 62B payload)",
                  (long long)((t1 - t0) / N), (long long)((t2 - t1) / N), N);

    /* 2. radio TX call latency: 8 NON multicast PINGs (API latency, not airtime) */
    if (mn_get_state() == MN_READY) {
        uint8_t ping[MN_ENV_HDR_LEN + sizeof(MN_SELFTEST_MAGIC)];
        enum { TXN = 8 };
        int sent = 0;
        int64_t t3 = esp_timer_get_time();
        for (int i = 0; i < TXN; i++) {
            int n = build_selftest_ping(ping, sizeof(ping));   /* fresh counter each */
            if (n > 0 && mn_ot_send(ping, (size_t)n, NULL, false) == 0) sent++;
        }
        int64_t t4 = esp_timer_get_time();
        mn_emit_event("# bench tx %d/%d NON multicast pings, %lldus/call (queue latency, not airtime)",
                      sent, TXN, (long long)((t4 - t3) / TXN));
    } else {
        mn_emit_event("# bench tx skipped (state=%s, need READY)", mn_state_name(s_state));
    }
}

void mn_stats_print(void) {
    mn_emit_event("# stats tx try=%lu ok=%lu err=%lu bytes=%llu",
                  (unsigned long)s_stats.tx_try, (unsigned long)s_stats.tx_ok,
                  (unsigned long)s_stats.tx_err, (unsigned long long)s_stats.tx_bytes);
    mn_emit_event("# stats rx msgs=%lu dup=%lu err=%lu bytes=%llu",
                  (unsigned long)s_stats.rx_msgs, (unsigned long)s_stats.rx_dup,
                  (unsigned long)s_stats.rx_err, (unsigned long long)s_stats.rx_bytes);
}

void mn_stats_reset(void) { memset(&s_stats, 0, sizeof(s_stats)); }

/* ---- saturation stress (STRESS <secs> <len>) ---- */
static volatile bool s_stress_running = false;
static uint32_t s_stress_secs, s_stress_len;

static void stress_task(void *arg) {
    (void)arg;
    uint32_t len = s_stress_len;
    uint8_t payload[MN_ENV_MAX_FRAME - MN_ENV_HDR_LEN];
    for (uint32_t i = 0; i < len; i++) payload[i] = 'a' + (i % 26);
    memcpy(payload, MN_STRESS_MARK, sizeof(MN_STRESS_MARK));

    uint32_t t0_try = s_stats.tx_try, t0_ok = s_stats.tx_ok, t0_err = s_stats.tx_err;
    int64_t start = esp_timer_get_time();
    int64_t deadline = start + (int64_t)s_stress_secs * 1000000;
    int64_t next_report = start + 30 * 1000000;

    while (esp_timer_get_time() < deadline) {
        /* seq number after the marker, for post-mortem debugging */
        uint32_t seq = s_stats.tx_try - t0_try;
        payload[3] = (uint8_t)(seq >> 24); payload[4] = (uint8_t)(seq >> 16);
        payload[5] = (uint8_t)(seq >> 8);  payload[6] = (uint8_t)seq;
        send_frame(MN_T_CHAT, payload, len, NULL, false);  /* bypass rate limit */
        if (esp_timer_get_time() >= next_report) {
            mn_emit_event("!STRESS t=%llds try=%lu ok=%lu err=%lu",
                          (long long)((esp_timer_get_time() - start) / 1000000),
                          (unsigned long)(s_stats.tx_try - t0_try),
                          (unsigned long)(s_stats.tx_ok - t0_ok),
                          (unsigned long)(s_stats.tx_err - t0_err));
            next_report += 30 * 1000000;
        }
        vTaskDelay(1);      /* 1 tick: keeps idle/OT tasks fed; radio saturates
                               long before this 1 kHz attempt ceiling */
    }

    int64_t dur_us = esp_timer_get_time() - start;
    uint32_t try_n = s_stats.tx_try - t0_try, ok_n = s_stats.tx_ok - t0_ok,
             err_n = s_stats.tx_err - t0_err;
    mn_emit_event("!STRESS_DONE secs=%lu len=%lu try=%lu ok=%lu err=%lu ok_per_sec=%lu.%02lu",
                  (unsigned long)(dur_us / 1000000), (unsigned long)len,
                  (unsigned long)try_n, (unsigned long)ok_n, (unsigned long)err_n,
                  (unsigned long)(ok_n * 100ULL * 1000000 / dur_us / 100),
                  (unsigned long)(ok_n * 100ULL * 1000000 / dur_us % 100));
    s_stress_running = false;
    vTaskDelete(NULL);
}

int mn_stress_start(uint32_t secs, uint32_t payload_len) {
    if (s_stress_running) return -1;
    if (secs < 1 || secs > 3600 ||
        payload_len < sizeof(MN_STRESS_MARK) + 4 ||
        payload_len > MN_ENV_MAX_FRAME - MN_ENV_HDR_LEN) return -2;
    s_stress_secs = secs;
    s_stress_len  = payload_len;
    s_stress_running = true;
    if (xTaskCreate(stress_task, "mn_stress", 4096, NULL, 4, NULL) != pdPASS) {
        s_stress_running = false;
        return -1;
    }
    return 0;
}

int mn_selftest(void) {
    int fails = 0;

    /* 1. envelope roundtrip (pure, always runs) */
    uint8_t frame[128];
    const char *probe = "selftest-payload";
    mn_envelope_t in = {
        .version = MN_ENV_VERSION, .type = MN_T_CHAT, .flags = MN_F_REQUIRES_ACK,
        .counter = 0x11223344, .epoch = 7, .selector = 0xbeef,
        .frag_total = 3, .frag_idx = 1, .msg_id = 0xcafe,
    };
    memcpy(in.sender_id, s_device_id, 4);
    int n = mn_env_pack(frame, sizeof(frame), &in, (const uint8_t *)probe, strlen(probe));
    mn_envelope_t out;
    bool env_ok = n == (int)(MN_ENV_HDR_LEN + strlen(probe)) &&
                  mn_env_unpack(&out, frame, (size_t)n) == 0 &&
                  out.type == in.type && out.flags == in.flags &&
                  out.counter == in.counter && out.epoch == in.epoch &&
                  out.selector == in.selector && out.frag_total == 3 &&
                  out.frag_idx == 1 && out.msg_id == in.msg_id &&
                  out.payload_len == strlen(probe) &&
                  !memcmp(out.payload, probe, out.payload_len);
    mn_emit_event("# selftest env-roundtrip %s", env_ok ? "ok" : "FAIL");
    if (!env_ok) fails++;

    /* Pump + loopback need the pump task to emit — which needs the TX mutex.
     * In FORTH mode the dispatcher holds it across this whole call, so the
     * pump would stall and time out. Detect and skip rather than false-fail. */
    if (s_tx_mutex &&
        xSemaphoreGetMutexHolder(s_tx_mutex) == xTaskGetCurrentTaskHandle()) {
        mn_emit_event("# selftest pump+loopback skipped (run SELFTEST from HCP mode, not ok>)");
        return fails ? -fails : 0;
    }

    /* 2. event pump: post a magic PING straight into the queue */
    xSemaphoreTake(s_selftest_sem, 0);            /* drain any stale give */
    n = build_selftest_ping(frame, sizeof(frame));
    s_selftest_armed = true;
    mn_post_rx(frame, (size_t)n, "::1", false);
    bool pump_ok = xSemaphoreTake(s_selftest_sem, pdMS_TO_TICKS(500)) == pdTRUE;
    s_selftest_armed = false;
    mn_emit_event("# selftest event-pump %s", pump_ok ? "ok" : "FAIL");
    if (!pump_ok) fails++;

    /* 3. CoAP loopback: magic PING to our OWN mesh-local EID — full trip
     * through OT/lwIP/CoAP and back into the /magnet handler */
    char eid[46];
    if (mn_ot_local_eid(eid, sizeof(eid)) == 0) {
        n = build_selftest_ping(frame, sizeof(frame));
        s_selftest_armed = true;
        bool loop_ok = false;
        if (mn_ot_send(frame, (size_t)n, eid, false) == 0) {
            loop_ok = xSemaphoreTake(s_selftest_sem, pdMS_TO_TICKS(2000)) == pdTRUE;
        }
        s_selftest_armed = false;
        mn_emit_event("# selftest coap-loopback %s (dst=%s)", loop_ok ? "ok" : "FAIL", eid);
        if (!loop_ok) fails++;
    } else {
        mn_emit_event("# selftest coap-loopback skipped (radio not up)");
    }

    mn_emit_event("# selftest %s (%d failure%s)",
                  fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? -fails : 0;
}
