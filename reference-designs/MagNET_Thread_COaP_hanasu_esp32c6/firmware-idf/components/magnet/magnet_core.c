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
#include "forth_core.h"
#include "sdkconfig.h"

/* E-B: single well-known channel; the selector constant stands in until the
 * §11.1 key hierarchy derives it from root_secret (E-Phase D). */
#define MN_DEFAULT_SELECTOR 0x6D61   /* "ma" */
#define MN_CHANNEL_NAME     "magnet"

static mn_state_t   s_state = MN_BOOTING;
static mn_putc_fn   s_putc  = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;

static uint8_t  s_device_id[4] = {0xde, 0xad, 0xbe, 0xef};
static uint32_t s_counter      = 0;   /* E-B: per-boot random base; E-D: NVS blocks */

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
    mn_emit_event("!STATE %s", mn_state_name(s));
}

/* ---- The single serialized writer. Lock the WHOLE line. ----
 * RECURSIVE mutex: the link dispatcher holds it across forth_eval() (FORTH
 * mode), and mn-* words called from Forth print through here — a plain mutex
 * would self-deadlock on the first `mn-status` at the ok> prompt. */
void mn_write_line(const char *line) {
    if (!s_putc) return;
    if (s_tx_mutex) xSemaphoreTakeRecursive(s_tx_mutex, portMAX_DELAY);
    for (const char *c = line; *c; ++c) s_putc(*c);
    s_putc('\r');
    s_putc('\n');
    if (s_tx_mutex) xSemaphoreGiveRecursive(s_tx_mutex);
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
    uint32_t last_seen_ticks;
} mn_peer_t;

static mn_peer_t s_peers[MN_PEER_MAX];

static void peer_seen(const uint8_t id[4], const char *ipv6) {
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < MN_PEER_MAX; i++) {
        if (s_peers[i].used && !memcmp(s_peers[i].id, id, 4)) {
            strlcpy(s_peers[i].ipv6, ipv6, sizeof(s_peers[i].ipv6));
            s_peers[i].last_seen_ticks = xTaskGetTickCount();
            return;
        }
        if (!s_peers[i].used && free_slot < 0) free_slot = i;
        if (s_peers[i].last_seen_ticks < s_peers[oldest].last_seen_ticks) oldest = i;
    }
    int slot = (free_slot >= 0) ? free_slot : oldest;
    bool is_new = (free_slot >= 0);
    s_peers[slot].used = true;
    memcpy(s_peers[slot].id, id, 4);
    strlcpy(s_peers[slot].ipv6, ipv6, sizeof(s_peers[slot].ipv6));
    s_peers[slot].last_seen_ticks = xTaskGetTickCount();
    if (is_new) {
        mn_emit_event("!PEER_JOIN %02x%02x%02x%02x - %s",
                      id[0], id[1], id[2], id[3], ipv6);
    }
}

static int peer_count(void) {
    int n = 0;
    for (int i = 0; i < MN_PEER_MAX; i++) if (s_peers[i].used) n++;
    return n;
}

/* ============== dedup (E-B: recent (sender,counter) cache) =================
 * Catches CoAP NON retransmits / mesh duplicates. The §11.1.6 monotonic
 * high-water table replaces this in E-Phase D (needs the persistent counter). */
#define MN_DEDUP_MAX 32

static struct { uint8_t id[4]; uint32_t counter; } s_dedup[MN_DEDUP_MAX];
static int s_dedup_next = 0;

static bool dedup_seen(const uint8_t id[4], uint32_t counter) {
    for (int i = 0; i < MN_DEDUP_MAX; i++) {
        if (s_dedup[i].counter == counter && !memcmp(s_dedup[i].id, id, 4))
            return true;
    }
    memcpy(s_dedup[s_dedup_next].id, id, 4);
    s_dedup[s_dedup_next].counter = counter;
    s_dedup_next = (s_dedup_next + 1) % MN_DEDUP_MAX;
    return false;
}

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
    if (env.flags & MN_F_ENCRYPTED) return;                   /* can't yet (E-D) */
    if (env.selector != MN_DEFAULT_SELECTOR) return;          /* not our channel */
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
        /* names arrive with the NAME verb (later); "-" until then */
        if (evt->was_multicast) mn_emit_event("!CHAT %s %s - %s", MN_CHANNEL_NAME, idhex, text);
        else                    mn_emit_event("!DM %s - %s", idhex, text);
        break;
    }
    default:
        mn_emit_event("# rx type=%u from=%s len=%u (unhandled in E-B)",
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
            mn_emit_event("!ROLE %s", evt.role);
            if (!strcmp(evt.role, "leader") || !strcmp(evt.role, "router") ||
                !strcmp(evt.role, "child")) {
                if (s_state != MN_READY) mn_set_state(MN_READY);
            } else if (!strcmp(evt.role, "detached")) {
                mn_set_state(s_state == MN_READY ? MN_DEGRADED : MN_ATTACHING);
            }
        } else if (evt.kind == MN_EVT_HEARTBEAT) {
            /* §4.6: heartbeat only once READY/DEGRADED */
            if (s_state == MN_READY || s_state == MN_DEGRADED) {
                mn_emit_event("!HEARTBEAT %s %llu %s %d",
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
    s_counter = esp_random();            /* E-D: NVS block-reserved counter */
    memset(s_peers, 0, sizeof(s_peers));
    memset(s_dedup, 0, sizeof(s_dedup));
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
static int send_type0(const char *msg, size_t len, const char *dst, bool con) {
    if (!msg || len == 0) return -1;
    if (len > MN_ENV_MAX_FRAME - MN_ENV_HDR_LEN) return -2;

    mn_envelope_t env = {
        .version    = MN_ENV_VERSION,
        .type       = MN_T_CHAT,
        .flags      = 0,
        .counter    = ++s_counter,
        .epoch      = 0,
        .selector   = MN_DEFAULT_SELECTOR,
        .frag_total = 1,
        .frag_idx   = 0,
        .msg_id     = (uint16_t)esp_random(),
    };
    memcpy(env.sender_id, s_device_id, 4);

    uint8_t frame[MN_ENV_MAX_FRAME];
    int n = mn_env_pack(frame, sizeof(frame), &env, (const uint8_t *)msg, len);
    if (n < 0) return n;
    s_stats.tx_try++;
    int rc = mn_ot_send(frame, (size_t)n, dst, con);
    if (rc == 0) { s_stats.tx_ok++; s_stats.tx_bytes += len; }
    else         s_stats.tx_err++;
    return rc;
}

int mn_chat(const char *msg, size_t len) {
    return send_type0(msg, len, NULL, false);   /* NON to the channel multicast */
}

int mn_dm(const char *peer_ipv6, const char *msg, size_t len) {
    return send_type0(msg, len, peer_ipv6, true);  /* CON to unicast peer */
}

void mn_status_print(void) {
    mn_emit_event("# state=%s role=%s channel=%s peers=%d id=%02x%02x%02x%02x",
                  mn_state_name(s_state), mn_ot_role_name(), MN_CHANNEL_NAME,
                  peer_count(),
                  s_device_id[0], s_device_id[1], s_device_id[2], s_device_id[3]);
}

void mn_peers_print(void) {
    int n = 0;
    uint32_t now = xTaskGetTickCount();
    for (int i = 0; i < MN_PEER_MAX; i++) {
        if (!s_peers[i].used) continue;
        mn_emit_event("# peer %02x%02x%02x%02x - %s last-seen=%lus ago",
                      s_peers[i].id[0], s_peers[i].id[1],
                      s_peers[i].id[2], s_peers[i].id[3],
                      s_peers[i].ipv6,
                      (unsigned long)((now - s_peers[i].last_seen_ticks) / configTICK_RATE_HZ));
        n++;
    }
    if (!n) mn_emit_event("# peers: none seen yet");
}

void mn_whoami_print(void) {
    /* E-Phase D: real device_id = SHA256(Ed25519 pubkey)[0:4]. E-B: EUI-64 tail. */
    mn_emit_event("# id=%02x%02x%02x%02x name=- fw=0.2.0-eb",
                  s_device_id[0], s_device_id[1], s_device_id[2], s_device_id[3]);
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
        .selector   = MN_DEFAULT_SELECTOR,
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
        .selector = MN_DEFAULT_SELECTOR, .msg_id = 1,
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
        mn_chat((const char *)payload, len);
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
