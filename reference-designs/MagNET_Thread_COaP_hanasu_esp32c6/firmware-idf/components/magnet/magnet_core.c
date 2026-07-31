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

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
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

#define MN_FW_VERSION "0.3.0-ec"
#define MN_NAME_MAX   16

/* E-D: the active channel — everything (selector, mcast, key) derives from
 * the credential's root_secret (§11.1). Default = "magnet" (well-known,
 * !WARN insecure per §11.5), derived once and cached in NVS. */
static mn_channel_t s_chan;
#define MN_CHANNEL_NAME (s_chan.name)

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
    if (!s_putc) return;
    if (s_terse && line[0] == '#') return;   /* MODE TERSE: drop comment lines */
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
#define MN_PEER_MAX 16

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
 * Unknown sender → trust-on-first-use. LRU-bounded. */
#define MN_DEDUP_MAX 16

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
static void queue_drain(void);
static void name_load(void);
static void counter_load(void);
static void chan_load(void);

/* ==================== event pump (§12.4) =================================== */
typedef enum { MN_EVT_RX, MN_EVT_ROLE, MN_EVT_HEARTBEAT } mn_evt_kind_t;

typedef struct {
    mn_evt_kind_t kind;
    /* RX */
    uint16_t len;
    bool     was_multicast;
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
    strlcpy(evt.src, src_ipv6, sizeof(evt.src));
    memcpy(evt.data, data, len);
    xQueueSend(s_evt_q, &evt, 0);        /* full queue → drop (mesh is lossy anyway) */
}

void mn_post_role(const char *role_name) {
    if (!s_evt_q) return;
    static mn_evt_t evt;                 /* same single-poster discipline */
    evt.kind = MN_EVT_ROLE;
    strlcpy(evt.role, role_name, sizeof(evt.role));
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

    if (!memcmp(env.sender_id, s_device_id, 4)) return;       /* self */
    if (env.selector != s_chan.selector) return;              /* not our channel */

    /* §11.1.5: decrypt + authenticate BEFORE any stateful processing */
    uint8_t pt[MN_ENV_MAX_FRAME];
    if (env.flags & MN_F_ENCRYPTED) {
        if (env.payload_len <= 8) { s_stats.rx_err++; return; }
        size_t ct_len = env.payload_len - 8;
        uint8_t nonce[13];
        mn_nonce_build(nonce, env.sender_id, env.counter, env.epoch, env.selector);
        if (mn_aead_decrypt(s_chan.epoch_key, nonce, evt->data,
                            env.payload, ct_len, pt,
                            env.payload + ct_len) != 0) {
            s_stats.rx_err++;                                 /* bad MIC */
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
    peer_seen(env.sender_id, evt->src);

    /* stress frames: count only, never emit (they'd flood the console) */
    if (env.type == MN_T_CHAT && env.payload_len >= sizeof(MN_STRESS_MARK) &&
        !memcmp(env.payload, MN_STRESS_MARK, sizeof(MN_STRESS_MARK))) {
        return;
    }

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
        } else {
            emit_class(MN_EC_CMD, "!CMD %s %s %u %u len=%u",
                       MN_CHANNEL_NAME, idhex, ns, cmd_id, plen);
        }
        break;
    }
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
                    announce_name();          /* tell the mesh who we are */
                    queue_drain();            /* replay DEGRADED-queued sends */
                }
            } else if (!strcmp(evt.role, "detached")) {
                mn_set_state(s_state == MN_READY ? MN_DEGRADED : MN_ATTACHING);
            }
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

/* =========================== bringup ====================================== */
void mn_core_init(void) {
    if (!s_tx_mutex) s_tx_mutex = xSemaphoreCreateRecursiveMutex();
    s_state = MN_BOOTING;
    memset(s_peers, 0, sizeof(s_peers));
    memset(s_dedup, 0, sizeof(s_dedup));
    name_load();                         /* NVS must be up (main inits it first) */
    counter_load();                      /* §11.1.6 block-reserved nonce counter */
    chan_load();                         /* active channel (default: "magnet")  */
    if (!strcmp(s_chan.name, "magnet"))
        mn_emit_event("!WARN default-channel-insecure use CHANNEL SET");
    if (!s_evt_q) {
        s_evt_q = xQueueCreate(8, sizeof(mn_evt_t));
        xTaskCreate(pump_task, "mn_pump", 4096, NULL, 5, NULL);
    }
    if (!s_selftest_sem) s_selftest_sem = xSemaphoreCreateBinary();
    if (!s_hb_timer) {
        s_hb_timer = xTimerCreate("mn_hb", pdMS_TO_TICKS(30 * 1000), pdTRUE,
                                  NULL, hb_timer_cb);
        xTimerStart(s_hb_timer, 0);
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

static int send_frame(uint8_t type, const uint8_t *payload, size_t len,
                      const char *dst, bool con) {
    if (!payload || len == 0) return -1;
    if (len > MN_ENV_MAX_FRAME - MN_ENV_HDR_LEN - 8) return -2;
    if (s_counter + 1 >= s_counter_limit && counter_reserve() != 0)
        return -5;               /* MUST NOT transmit without a fresh nonce */

    mn_envelope_t env = {
        .version    = MN_ENV_VERSION,
        .type       = type,
        .flags      = s_chan.set ? MN_F_ENCRYPTED : 0,
        .counter    = ++s_counter,
        .epoch      = 0,
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

    s_stats.tx_try++;
    int rc = mn_ot_send(frame, (size_t)n, dst, con);
    if (rc == 0) { s_stats.tx_ok++; s_stats.tx_bytes += len; }
    else         s_stats.tx_err++;
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
    chan_persist();
    mn_ot_set_mcast(s_chan.mcast_suffix);        /* no-op if radio not up */
    memset(s_dedup, 0, sizeof(s_dedup));         /* new channel, new peers */
    memset(s_peers, 0, sizeof(s_peers));
    if (info) snprintf(info, cap, "%s selector=%04x path=%c",
                       s_chan.name, s_chan.selector, s_chan.cred_path);
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

void mn_status_line(char *buf, size_t cap) {
    snprintf(buf, cap, "state=%s role=%s channel=%s peers=%d name=%s id=%02x%02x%02x%02x",
             mn_state_name(s_state), mn_ot_role_name(), MN_CHANNEL_NAME,
             peer_count(), s_name,
             s_device_id[0], s_device_id[1], s_device_id[2], s_device_id[3]);
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
