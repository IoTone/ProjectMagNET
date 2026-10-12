// ear_atvv — ATVV v1.0 remote side. See ear_atvv.h / docs/Ear-ATVV-profile.md.
//
// Threading: NimBLE access/GAP callbacks only enqueue events; one control
// task owns all state transitions (start/stop block on the audio task, which
// must never happen on the NimBLE host task).
//
// Audio path: every encoded frame goes into a ring (3 s). While the mic runs
// the audio task drains it; after the mic stops, the control task drains the
// tail before AUDIO_STOP. Draining stops while NimBLE's mbuf pool is low and
// resumes later, so a slow link delays frames instead of dropping them, and
// GATT writes (MIC_EXTEND) always find a buffer. The ring also holds the
// utterance while a talk press waits for its host to reconnect.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "host/ble_hs.h"

#include "ear_atvv.h"
#include "ear_audio.h"
#include "ear_ble.h"

static const char *TAG = "ear_atvv";

#ifndef EAR_DEV_JUSTWORKS
#define EAR_DEV_JUSTWORKS 0
#endif
#if EAR_DEV_JUSTWORKS
#define CHR_F_AUTHEN_W 0
#define CHR_F_AUTHEN_R 0
#else
#define CHR_F_AUTHEN_W BLE_GATT_CHR_F_WRITE_AUTHEN
#define CHR_F_AUTHEN_R BLE_GATT_CHR_F_READ_AUTHEN
#endif

/* ---- wire constants (v1.0) ---- */
#define OP_GET_CAPS     0x0A   /* TX */
#define OP_MIC_OPEN     0x0C
#define OP_MIC_CLOSE    0x0D
#define OP_MIC_EXTEND   0x0E
#define OP_AUDIO_STOP   0x00   /* CTL */
#define OP_AUDIO_START  0x04
#define OP_AUDIO_SYNC   0x0A
#define OP_CAPS_RESP    0x0B
#define OP_MIC_OPEN_ERR 0x0C

#define VERSION_1_0     0x0100
#define CODEC_ADPCM_16K 0x02
#define MODEL_HTT       0x03

#define START_MIC_OPEN  0x00
#define START_HTT       0x03
#define STOP_MIC_CLOSE  0x00
#define STOP_HTT_RELEASE 0x02
#define STOP_UPCOMING   0x04
#define STOP_TIMEOUT    0x08

#define ERR_REMOTE_NOT_ACTIVE   0x0F02
#define ERR_NOTIFY_DISABLED     0x0F03
#define ERR_HTT_IN_PROGRESS     0x0F80
#define ERR_INTERNAL            0x0FFF

#define FRAME_PREFERRED  128    /* ADPCM bytes = 256 samples = 16 ms @ 16 kHz */
#define SYNC_EVERY       25     /* AUDIO_SYNC every 0.4 s, plus after any drop */
#define RING_FRAMES      192    /* x 16 ms = 3.07 s queued on the Ear */
#define MSYS_RESERVE     8      /* msys_1 blocks left free for ATT responses/CTL */
#define DRAIN_BURST      6      /* frames per drain pass (catch-up <= 600 frames/s) */
#define WAKE_TIMEOUT_US  (6 * 1000000LL)   /* talk press waits this long for a host */
#define FLUSH_TIMEOUT_US (3 * 1000000LL)   /* tail drain before AUDIO_STOP */
#define NO_HOST_SHOW_US  (3 * 1000000LL)
#define MIC_OPEN_TIMEOUT_US (15 * 1000000LL)   /* host must MIC_EXTEND */

/* AB5E000x-5A21-4F05-BC7D-AF01F617B664, little-endian for NimBLE */
#define ATVV_UUID(x) BLE_UUID128_INIT(0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC, \
                                      0x05, 0x4F, 0x21, 0x5A, (x), 0x00, 0x5E, 0xAB)
static const ble_uuid128_t U_SVC = ATVV_UUID(0x01);
static const ble_uuid128_t U_TX  = ATVV_UUID(0x02);
static const ble_uuid128_t U_RX  = ATVV_UUID(0x03);
static const ble_uuid128_t U_CTL = ATVV_UUID(0x04);
static uint16_t h_rx, h_ctl;

/* ---- peers ---- */
typedef struct {
    bool     used, rx_sub, ctl_sub;
    uint16_t conn, mtu, caps_frame, itvl;
} peer_t;
static peer_t s_peer[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];

static peer_t *peer_get(uint16_t conn) {
    for (int i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++)
        if (s_peer[i].used && s_peer[i].conn == conn) return &s_peer[i];
    return NULL;
}
static bool peer_authenticated(uint16_t conn) {
    struct ble_gap_conn_desc d;
    return ble_gap_conn_find(conn, &d) == 0 &&
           d.sec_state.encrypted && d.sec_state.bonded &&
           (d.sec_state.authenticated || EAR_DEV_JUSTWORKS);
}
/* Ready = subscribed AND the host app has sent GET_CAPS on this link. A
 * bonded host's subscriptions are restored by the stack at encryption,
 * before its app is listening; GET_CAPS is the first thing an ATVV host
 * app sends, so it is the reliable "app is up" signal (2026-10-07). */
static bool peer_ready(const peer_t *p) {
    return p && p->used && p->rx_sub && p->ctl_sub && p->caps_frame &&
           peer_authenticated(p->conn);
}

/* ---- stream state (owned by the control task; sink reads) ---- */
static volatile bool     s_live;        /* session open: START sent or pending */
static volatile bool     s_bound;       /* AUDIO_START sent to s_audio_conn */
static volatile bool     s_capturing;   /* mic running */
static volatile bool     s_need_sync;
static volatile uint16_t s_audio_conn;
static volatile uint32_t s_sent, s_dropped, s_stalls;
static uint8_t           s_reason, s_sid, s_next_htt_sid = 1;
static uint8_t           s_stop_reason = 0xFF;   /* 0xFF: no stop pending */
static uint16_t          s_frame_bytes;
static int64_t           s_deadline;    /* MIC_OPEN timeout */
static int64_t           s_wake_until;  /* talk press: give up waiting for a host */
static int64_t           s_flush_until; /* tail drain limit */
static int64_t           s_nohost_until;

/* ---- frame ring: written by the sink, drained by sink or control task ---- */
typedef struct { uint16_t fno; int16_t pred; uint8_t idx; } fmeta_t;
static uint8_t          *s_ring;        /* RING_FRAMES * FRAME_PREFERRED */
static fmeta_t           s_meta[RING_FRAMES];
static volatile uint32_t s_head, s_tail;
static SemaphoreHandle_t s_drain_mx;
static uint16_t          s_synced_fno = 0xFFFF;   /* last frame AUDIO_SYNC'd */

/* ---- events ---- */
enum { EV_TX, EV_BTN_DOWN, EV_BTN_UP, EV_DISC };
typedef struct { uint8_t type, len; uint16_t conn; uint8_t data[8]; } ev_t;
static QueueHandle_t s_q;

static void post(uint8_t type, uint16_t conn, const uint8_t *d, uint8_t len) {
    ev_t e = { .type = type, .len = len, .conn = conn };
    if (d && len) memcpy(e.data, d, len > sizeof(e.data) ? sizeof(e.data) : len);
    if (xQueueSend(s_q, &e, 0) != pdTRUE) ESP_LOGW(TAG, "event queue full");
}

/* ---- notify helpers ---- */
static int notify(uint16_t conn, uint16_t h, const void *buf, size_t len) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (!om) return BLE_HS_ENOMEM;
    return ble_gatts_notify_custom(conn, h, om);   /* consumes om */
}
/* Control messages must not be lost to a momentarily full buffer pool:
 * retry for up to ~100 ms (control task only). */
static void ctl(uint16_t conn, const uint8_t *b, size_t n) {
    peer_t *p = peer_get(conn);
    if (!p || !p->ctl_sub) return;
    int rc;
    for (int i = 0; (rc = notify(conn, h_ctl, b, n)) == BLE_HS_ENOMEM && i < 20; i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (rc) ESP_LOGW(TAG, "CTL 0x%02x notify rc=%d", b[0], rc);
}
static void mic_open_error(uint16_t conn, uint16_t code) {
    uint8_t b[3] = { OP_MIC_OPEN_ERR, code >> 8, code & 0xFF };
    ESP_LOGW(TAG, "MIC_OPEN_ERROR 0x%04x", code);
    ctl(conn, b, sizeof(b));
}

static uint16_t frame_for_mtu(uint16_t mtu) {
    int pay = (int)mtu - 3;
    if (pay >= FRAME_PREFERRED) return FRAME_PREFERRED;
    if (pay < 20) pay = 20;
    return (uint16_t)(pay & ~1);
}

/* Free blocks in msys_1, the pool notifications and ATT responses are both
 * allocated from (os_msys_num_free() sums every pool, hiding msys_1 running
 * dry). With 12 blocks it hit 0 while streaming and host writes failed
 * "Insufficient Resource"; sdkconfig.defaults now sets 24 and this keeps 8
 * back for responses/CTL (HW, 2026-10-07). */
static struct os_mempool *s_msys1;
static int msys1_free(void) {
    if (!s_msys1) {
        struct os_mempool_info omi;
        for (struct os_mempool *mp = os_mempool_info_get_next(NULL, &omi); mp;
             mp = os_mempool_info_get_next(mp, &omi))
            if (strcmp(omi.omi_name, "msys_1") == 0) { s_msys1 = mp; break; }
        if (!s_msys1) return os_msys_num_free();
    }
    return s_msys1->mp_num_free;
}

static void ring_reset(void) { s_head = s_tail = 0; s_synced_fno = 0xFFFF; }
static uint32_t ring_count(void) { return s_head - s_tail; }

/* Send queued frames until the ring empties or NimBLE runs low on buffers.
 * Frames stay queued on failure: a slow link delays audio, never drops it
 * (unless the ring overflows). */
static void drain(void) {
    if (!s_bound) return;
    xSemaphoreTake(s_drain_mx, portMAX_DELAY);
    for (int k = 0; k < DRAIN_BURST && s_tail != s_head && s_bound; k++) {
        if (msys1_free() < MSYS_RESERVE) { s_stalls++; break; }
        uint32_t slot = s_tail % RING_FRAMES;
        const fmeta_t *m = &s_meta[slot];
        if ((s_need_sync || (m->fno % SYNC_EVERY) == 0) && s_synced_fno != m->fno) {
            uint8_t b[7] = { OP_AUDIO_SYNC, CODEC_ADPCM_16K, m->fno >> 8, m->fno & 0xFF,
                             (uint16_t)m->pred >> 8, (uint16_t)m->pred & 0xFF, m->idx };
            if (notify(s_audio_conn, h_ctl, b, sizeof(b)) != 0) break;
            s_synced_fno = m->fno;
            s_need_sync = false;
        }
        if (notify(s_audio_conn, h_rx, s_ring + slot * FRAME_PREFERRED, s_frame_bytes) != 0) break;
        s_sent++;
        s_tail++;
    }
    xSemaphoreGive(s_drain_mx);
}

/* ---- audio sink (audio task) ---- */
static void sink(const uint8_t *a, size_t n, const ima_state_t *st, uint16_t fno, void *ctx) {
    (void)ctx;
    if (!s_live || n > FRAME_PREFERRED) return;
    xSemaphoreTake(s_drain_mx, portMAX_DELAY);
    if (ring_count() >= RING_FRAMES) {          /* full: lose the oldest */
        s_tail++;
        s_dropped++;
        s_need_sync = true;
    }
    uint32_t slot = s_head % RING_FRAMES;
    memcpy(s_ring + slot * FRAME_PREFERRED, a, n);
    s_meta[slot] = (fmeta_t){ fno, st->predictor, st->step_index };
    s_head++;
    xSemaphoreGive(s_drain_mx);
    drain();
}

/* ---- transitions (control task only) ---- */
static void finish(bool tell) {
    if (tell && s_bound) {
        uint8_t b[2] = { OP_AUDIO_STOP, s_stop_reason };
        ctl(s_audio_conn, b, sizeof(b));
    }
    ESP_LOGI(TAG, "AUDIO_STOP reason=0x%02x sent=%lu dropped=%lu left=%lu stalls=%lu msys1 free=%d/%d min=%d",
             s_stop_reason, (unsigned long)s_sent, (unsigned long)s_dropped,
             (unsigned long)ring_count(), (unsigned long)s_stalls, msys1_free(),
             s_msys1 ? s_msys1->mp_num_blocks : -1, s_msys1 ? s_msys1->mp_min_free : -1);
    s_stalls = 0;
    s_live = s_bound = false;
    s_stop_reason = 0xFF;
    ring_reset();
    ear_ble_wake(false);
}

static void capture_stop(void) {
    if (!s_capturing) return;
    s_capturing = false;
    ear_audio_stop();
}

/* End a session. tell=false (link gone / pre-empted): drop the tail now.
 * Otherwise the control loop drains the tail, then sends AUDIO_STOP. */
static void stream_stop(uint8_t reason, bool tell) {
    if (!s_live) return;
    capture_stop();
    s_stop_reason = reason;
    if (!tell) { finish(false); return; }
    s_flush_until = esp_timer_get_time() + FLUSH_TIMEOUT_US;
}

/* AUDIO_START to a ready host; frames queued so far follow it. */
static bool bind(uint16_t conn) {
    peer_t *p = peer_get(conn);
    if (!peer_ready(p)) return false;
    uint16_t fb = p->caps_frame;
    if (fb != s_frame_bytes) {
        ESP_LOGW(TAG, "host frame %u B != stream %u B (MTU %u)", fb, s_frame_bytes, p->mtu);
        return false;
    }
    s_audio_conn = conn;
    uint8_t b[4] = { OP_AUDIO_START, s_reason, CODEC_ADPCM_16K, s_sid };
    ctl(conn, b, sizeof(b));

    /* 15-30 ms interval: 62.5 frames/s needs 1-2 notifications per event. */
    struct ble_gap_upd_params up = { .itvl_min = 12, .itvl_max = 24, .latency = 0,
                                     .supervision_timeout = 400 };
    ble_gap_update_params(conn, &up);
    s_need_sync = true;
    s_synced_fno = 0xFFFF;
    if (!s_capturing) s_flush_until = esp_timer_get_time() + FLUSH_TIMEOUT_US;
    s_bound = true;
    ear_ble_wake(false);
    ESP_LOGI(TAG, "AUDIO_START reason=0x%02x sid=%u frame=%u B mtu=%u queued=%lu",
             s_reason, s_sid, s_frame_bytes, p->mtu, (unsigned long)ring_count());
    return true;
}

static int ready_peer(uint16_t *conn) {
    for (int i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++)
        if (peer_ready(&s_peer[i])) { *conn = s_peer[i].conn; return 1; }
    return 0;
}

/* Open a session. conn = host to bind now, or <0: no host yet (talk press
 * while disconnected) — buffer and advertise fast until one shows up. */
static int stream_start(int conn, uint8_t reason, uint8_t sid) {
    if (conn >= 0) {
        peer_t *p = peer_get((uint16_t)conn);
        if (!peer_ready(p)) return -1;
        s_frame_bytes = p->caps_frame;
    } else {
        s_frame_bytes = FRAME_PREFERRED;
    }
    s_reason = reason; s_sid = sid;
    s_sent = s_dropped = 0;
    s_stop_reason = 0xFF;
    ring_reset();
    s_deadline = esp_timer_get_time() + MIC_OPEN_TIMEOUT_US;
    s_live = true;
    if (conn >= 0) bind((uint16_t)conn);
    else {
        s_wake_until = esp_timer_get_time() + WAKE_TIMEOUT_US;
        ear_ble_wake(true);
        ESP_LOGI(TAG, "talk pressed, no host: buffering, advertising fast");
    }
    if (ear_audio_start(s_frame_bytes, sink, NULL) != 0) {
        s_stop_reason = STOP_MIC_CLOSE;
        finish(true);
        return -2;
    }
    s_capturing = true;
    return 0;
}

static void handle_tx(uint16_t conn, const uint8_t *d, uint8_t len) {
    peer_t *p = peer_get(conn);
    if (!p || len < 1) return;
    switch (d[0]) {
    case OP_GET_CAPS: {
        p->caps_frame = frame_for_mtu(p->mtu);
        uint8_t b[9] = { OP_CAPS_RESP, VERSION_1_0 >> 8, VERSION_1_0 & 0xFF,
                         CODEC_ADPCM_16K, MODEL_HTT,
                         p->caps_frame >> 8, p->caps_frame & 0xFF,
                         0x00 /* extra config */, 0x00 /* reserved */ };
        ESP_LOGI(TAG, "GET_CAPS (host ver %02x%02x) -> v1.0, 16k ADPCM, HTT, frame %u",
                 len > 2 ? d[1] : 0, len > 2 ? d[2] : 0, p->caps_frame);
        ctl(conn, b, sizeof(b));
        break;
    }
    case OP_MIC_OPEN:
        if (s_live) {
            if (s_reason == START_HTT) { mic_open_error(conn, ERR_HTT_IN_PROGRESS); break; }
            s_deadline = esp_timer_get_time() + MIC_OPEN_TIMEOUT_US;   /* already open */
            break;
        }
        if (!p->rx_sub || !p->ctl_sub) { mic_open_error(conn, ERR_NOTIFY_DISABLED); break; }
        if (!p->caps_frame) p->caps_frame = frame_for_mtu(p->mtu);   /* host skipped GET_CAPS */
        if (stream_start(conn, START_MIC_OPEN, 0x00) != 0) mic_open_error(conn, ERR_INTERNAL);
        break;
    case OP_MIC_CLOSE:
        if (s_live && s_bound && conn == s_audio_conn && s_stop_reason == 0xFF)
            stream_stop(STOP_MIC_CLOSE, true);
        break;
    case OP_MIC_EXTEND:
        if (s_live && conn == s_audio_conn) s_deadline = esp_timer_get_time() + MIC_OPEN_TIMEOUT_US;
        break;
    default:
        ESP_LOGW(TAG, "unknown TX opcode 0x%02x", d[0]);
    }
}

static void ctrl_task(void *arg) {
    (void)arg;
    for (;;) {
        ev_t e;
        if (xQueueReceive(s_q, &e, pdMS_TO_TICKS(s_live ? 10 : 100)) == pdTRUE) {
            switch (e.type) {
            case EV_TX:
                handle_tx(e.conn, e.data, e.len);
                break;
            case EV_BTN_DOWN: {
                if (s_live) {
                    if (s_reason == START_HTT) break;       /* already talking / flushing */
                    capture_stop();                         /* HTT pre-empts MIC_OPEN */
                    s_stop_reason = STOP_UPCOMING;
                    finish(true);
                }
                uint16_t conn;
                int have = ready_peer(&conn);
                if (!have && ear_ble_bond_count() == 0) {
                    ESP_LOGI(TAG, "talk pressed, not paired");
                    break;
                }
                uint8_t sid = s_next_htt_sid;
                s_next_htt_sid = s_next_htt_sid >= 0x7F ? 1 : s_next_htt_sid + 1;
                stream_start(have ? conn : -1, START_HTT, sid);
                break;
            }
            case EV_BTN_UP:
                if (s_live && s_reason == START_HTT && s_stop_reason == 0xFF)
                    stream_stop(STOP_HTT_RELEASE, true);
                break;
            case EV_DISC:
                if (s_live && s_bound && e.conn == s_audio_conn) {
                    capture_stop();
                    s_bound = false;
                    finish(false);
                }
                break;
            }
        }
        if (!s_live) continue;
        int64_t now = esp_timer_get_time();

        if (!s_bound) {                                   /* talk press waiting for a host */
            uint16_t conn;
            if (ready_peer(&conn) && bind(conn)) {
                /* bound: the sink (or the flush below) now drains the queue */
            } else if (now > s_wake_until) {
                ESP_LOGW(TAG, "no host within %d s: discarding %lu frames",
                         (int)(WAKE_TIMEOUT_US / 1000000), (unsigned long)ring_count());
                capture_stop();
                s_nohost_until = now + NO_HOST_SHOW_US;
                finish(false);
                continue;
            }
        }
        if (s_bound && !s_capturing) {                    /* mic stopped: flush the tail */
            drain();
            if (ring_count() == 0 || now > s_flush_until) {
                if (s_stop_reason == 0xFF) s_stop_reason = STOP_MIC_CLOSE;
                finish(true);
                continue;
            }
        }
        if (s_capturing && s_reason == START_MIC_OPEN && now > s_deadline)
            stream_stop(STOP_TIMEOUT, true);
    }
}

/* ---- GATT ---- */
static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[8]; uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    if (rc && rc != BLE_HS_EMSGSIZE) return BLE_ATT_ERR_UNLIKELY;
    post(EV_TX, conn, buf, (uint8_t)len);
    return 0;
}
static int ro_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0;   /* readable but empty — some hosts read before subscribing */
}

static const struct ble_gatt_chr_def s_chrs[] = {
    { .uuid = &U_TX.u, .access_cb = tx_access,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
               BLE_GATT_CHR_F_WRITE_ENC | CHR_F_AUTHEN_W },
    { .uuid = &U_RX.u, .access_cb = ro_access, .val_handle = &h_rx,
      .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ |
               BLE_GATT_CHR_F_READ_ENC | CHR_F_AUTHEN_R },
    { .uuid = &U_CTL.u, .access_cb = ro_access, .val_handle = &h_ctl,
      .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ |
               BLE_GATT_CHR_F_READ_ENC | CHR_F_AUTHEN_R },
    { 0 },
};

const struct ble_gatt_svc_def ear_atvv_svc = {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &U_SVC.u,
    .characteristics = s_chrs,
};

/* ---- API ---- */
int ear_atvv_init(void) {
    s_q = xQueueCreate(16, sizeof(ev_t));
    s_drain_mx = xSemaphoreCreateMutex();
    s_ring = heap_caps_malloc(RING_FRAMES * FRAME_PREFERRED, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_q || !s_drain_mx || !s_ring) return -1;
    return xTaskCreate(ctrl_task, "atvv", 4096, NULL, 7, NULL) == pdPASS ? 0 : -1;
}

void ear_atvv_on_connect(uint16_t conn) {
    for (int i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++)
        if (!s_peer[i].used) { s_peer[i] = (peer_t){ .used = true, .conn = conn, .mtu = 23 }; return; }
}
void ear_atvv_on_disconnect(uint16_t conn) {
    post(EV_DISC, conn, NULL, 0);
    peer_t *p = peer_get(conn);
    if (p) p->used = false;
}
void ear_atvv_on_subscribe(uint16_t conn, uint16_t attr, bool on) {
    peer_t *p = peer_get(conn);
    if (!p) return;
    if (attr == h_rx)  p->rx_sub = on;
    if (attr == h_ctl) p->ctl_sub = on;
    if (!on && s_live && conn == s_audio_conn) post(EV_DISC, conn, NULL, 0);
}
void ear_atvv_on_mtu(uint16_t conn, uint16_t mtu) {
    peer_t *p = peer_get(conn);
    if (p) p->mtu = mtu;
}
void ear_atvv_on_enc(uint16_t conn) { (void)conn; }
void ear_atvv_on_conn_params(uint16_t conn, uint16_t itvl) {
    peer_t *p = peer_get(conn);
    if (p) p->itvl = itvl;
}

void ear_atvv_button(bool down) { post(down ? EV_BTN_DOWN : EV_BTN_UP, 0, NULL, 0); }

void ear_atvv_status(atvv_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->state = ATVV_IDLE;
    for (int i = 0; i < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++)
        if (peer_ready(&s_peer[i])) {
            out->state = ATVV_READY; out->mtu = s_peer[i].mtu;
            out->conn_itvl_x100 = s_peer[i].itvl * 125;
        }
    out->no_host = s_nohost_until > esp_timer_get_time();
    if (s_live) {
        out->state = ATVV_LIVE;
        out->waiting_host = !s_bound;
        out->capturing = s_capturing;
        out->buffered_ms = (uint16_t)(ring_count() * 16);
        out->reason = s_reason;
        out->frame_bytes = s_frame_bytes;
        peer_t *p = s_bound ? peer_get(s_audio_conn) : NULL;
        if (p) { out->mtu = p->mtu; out->conn_itvl_x100 = p->itvl * 125; }
    }
    out->frames_sent = s_sent;
    out->frames_dropped = s_dropped;
}
