/*
 * magnet_xfer.c — Type 6 extended transfer (E-H, docs/EXTENDED-TRANSFER.md).
 *
 * Everything the E-B saturation tables taught, applied: CON unicast only
 * (fragmented multicast loses ~7% quiet and collapses loaded), paced under the
 * per-node OT acceptance ceiling (8 chunks per 250 ms tick), and app-layer
 * recovery by window bitmap because CoAP CON covers a chunk, not a transfer.
 *
 * The node is a modem. Outbound, it buffers ONE window so it can retransmit
 * without re-asking the host; inbound it keeps a bitmap and forwards every
 * chunk up the host link the moment it arrives — the host reassembles by
 * index. Neither side ever holds the file.
 *
 * Task discipline: mn_xfer_begin/data/abort/status run on the HCP dispatcher
 * (or BLE worker); mn_xfer_on_rx/tick run on the pump task. One mutex guards
 * the two sessions. Lock order matches the rest of the core: xfer mutex →
 * (TX mutex | OT lock), never the reverse — nothing here is called from OT
 * callback context.
 */
#include "magnet.h"
#include "magnet_xfer.h"
#include "magnet_envelope.h"

#include <string.h>
#include <stdio.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "mbedtls/base64.h"

/* subtypes (payload byte 0) */
#define X_INIT   0x01
#define X_DATA   0x02
#define X_STATUS 0x03
#define X_ABORT  0x04

/* STATUS codes */
#define XC_PROGRESS 0
#define XC_COMPLETE 1
#define XC_REFUSED  2
#define XC_BUSY     3

#define X_META_MAX        64
#define X_TICK_MS         250
#define X_CHUNKS_PER_TICK 8
#define X_INIT_RETRY_US   (1000LL * 1000)
#define X_INIT_TRIES_MAX  6
#define X_STATUS_WAIT_US  (2500LL * 1000)   /* sender: silence → resend round */
#define X_ROUNDS_MAX      6
#define X_RX_GAP_US       (700LL * 1000)    /* receiver: stall → NACK STATUS  */
#define X_IDLE_ABORT_US   (30LL * 1000 * 1000)

static struct {
    bool     active;
    bool     accepted;                  /* any STATUS heard for this xid      */
    uint16_t xid;
    char     peer[46];
    uint32_t total_len;
    uint16_t total_chunks;
    uint16_t base;                      /* window base chunk index            */
    uint16_t fill;                      /* chunks the host has buffered       */
    uint64_t acked;                     /* bit i = chunk base+i acked         */
    uint64_t sent;                      /* bit i = transmitted, awaiting ack  */
    uint8_t  rounds;
    uint8_t  init_tries;
    int64_t  last_init_us;
    int64_t  last_status_us;            /* last STATUS heard from receiver    */
    uint8_t  meta_len;
    char     meta[X_META_MAX];
    uint8_t  buf[MN_XFER_WINDOW][MN_XFER_CHUNK];
} s_tx;

static struct {
    bool     active;
    uint16_t xid;
    uint8_t  peer_id[4];
    char     src[46];
    uint32_t total_len;
    uint16_t total_chunks;
    uint16_t chunk_len;
    uint16_t got;
    uint16_t report_base;               /* last window base we sent STATUS for */
    int64_t  last_rx_us;
    int64_t  last_status_us;
    uint8_t  bitmap[MN_XFER_MAX_CHUNKS / 8];
} s_rx;

static SemaphoreHandle_t s_mx = NULL;
static TimerHandle_t     s_tick = NULL;

static void tick_cb(TimerHandle_t t) { (void)t; mn_post_xfer_tick(); }

void mn_xfer_init(void) {
    s_mx = xSemaphoreCreateMutex();
    s_tick = xTimerCreate("mn_xfer", pdMS_TO_TICKS(X_TICK_MS), pdTRUE, NULL, tick_cb);
}

static void tick_ensure_running(void) {
    if (s_tick && !xTimerIsTimerActive(s_tick)) xTimerStart(s_tick, 0);
}

/* ---- small helpers (mutex held unless noted) ---- */

static uint16_t tx_chunk_len(uint16_t idx) {
    return (idx == s_tx.total_chunks - 1)
               ? (uint16_t)(s_tx.total_len - (uint32_t)idx * MN_XFER_CHUNK)
               : MN_XFER_CHUNK;
}

static uint16_t tx_window_n(void) {
    uint16_t left = (uint16_t)(s_tx.total_chunks - s_tx.base);
    return left < MN_XFER_WINDOW ? left : MN_XFER_WINDOW;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int send_init(void) {
    uint8_t pl[12 + X_META_MAX];
    pl[0] = X_INIT;
    put_u16(pl + 1, s_tx.xid);
    put_u32(pl + 3, s_tx.total_len);
    put_u16(pl + 7, s_tx.total_chunks);
    put_u16(pl + 9, MN_XFER_CHUNK);
    pl[11] = s_tx.meta_len;
    if (s_tx.meta_len) memcpy(pl + 12, s_tx.meta, s_tx.meta_len);
    return mn_xfer_send_frame(pl, (size_t)12 + s_tx.meta_len, s_tx.peer);
}

static int send_status(const char *dst, uint16_t xid, uint16_t base,
                       uint64_t bitmap, uint8_t code) {
    uint8_t pl[14];
    pl[0] = X_STATUS;
    put_u16(pl + 1, xid);
    put_u16(pl + 3, base);
    for (int i = 0; i < 8; i++) pl[5 + i] = (uint8_t)(bitmap >> (8 * i));
    pl[13] = code;
    return mn_xfer_send_frame(pl, 14, dst);
}

static int send_abort(const char *dst, uint16_t xid, uint8_t reason) {
    uint8_t pl[4] = { X_ABORT, 0, 0, reason };
    put_u16(pl + 1, xid);
    return mn_xfer_send_frame(pl, 4, dst);
}

static void tx_fail(const char *reason) {
    mn_emit_event("!XFER_FAIL %04x %s", s_tx.xid, reason);
    s_tx.active = false;
}

static void rx_close(void) { s_rx.active = false; }

static void rx_fail(const char *reason) {
    mn_emit_event("!XFER_FAIL %04x %s", s_rx.xid, reason);
    rx_close();
}

/* first chunk index the receiver is still missing (== total when complete) */
static uint16_t rx_first_missing(void) {
    for (uint16_t i = 0; i < s_rx.total_chunks; i++)
        if (!(s_rx.bitmap[i >> 3] & (1u << (i & 7)))) return i;
    return s_rx.total_chunks;
}

/* 64-bit received-picture for the window starting at base */
static uint64_t rx_window_bitmap(uint16_t base) {
    uint64_t m = 0;
    for (uint16_t i = 0; i < 64 && (uint16_t)(base + i) < s_rx.total_chunks; i++)
        if (s_rx.bitmap[(base + i) >> 3] & (1u << ((base + i) & 7)))
            m |= (1ULL << i);
    return m;
}

static void rx_send_status(uint8_t code) {
    uint16_t fm   = rx_first_missing();
    uint16_t base = (uint16_t)(fm - (fm % MN_XFER_WINDOW));
    send_status(s_rx.src, s_rx.xid, base, rx_window_bitmap(base), code);
    s_rx.report_base    = base;
    s_rx.last_status_us = esp_timer_get_time();
}

/* ---- host (sender) side ---- */

int mn_xfer_begin(const char *peer_ipv6, uint32_t total_len,
                  const char *meta, char *info, size_t cap) {
    if (!peer_ipv6 || total_len == 0) return -2;
    uint32_t chunks = (total_len + MN_XFER_CHUNK - 1) / MN_XFER_CHUNK;
    if (chunks > MN_XFER_MAX_CHUNKS) return -2;

    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_tx.active) { xSemaphoreGive(s_mx); return -1; }

    memset(&s_tx, 0, sizeof(s_tx));
    s_tx.active       = true;
    s_tx.xid          = (uint16_t)esp_random();
    s_tx.total_len    = total_len;
    s_tx.total_chunks = (uint16_t)chunks;
    strlcpy(s_tx.peer, peer_ipv6, sizeof(s_tx.peer));

    /* INIT carries the meta string (≤64 B); retries from the tick reuse it */
    size_t mlen = meta ? strlen(meta) : 0;
    if (mlen > X_META_MAX) mlen = X_META_MAX;
    s_tx.meta_len = (uint8_t)mlen;
    if (mlen) memcpy(s_tx.meta, meta, mlen);

    int rc = send_init();
    s_tx.init_tries   = 1;
    s_tx.last_init_us = esp_timer_get_time();
    if (rc == -2) { s_tx.active = false; xSemaphoreGive(s_mx); return -3; }
    /* other send errors: the tick retries INIT — don't fail a begin on a
     * transient NO_BUFS */

    snprintf(info, cap, "xid=%04x chunks=%u chunk=%u window=%u",
             s_tx.xid, s_tx.total_chunks, MN_XFER_CHUNK, MN_XFER_WINDOW);
    tick_ensure_running();
    xSemaphoreGive(s_mx);
    return 0;
}

int mn_xfer_data_b64(const char *b64) {
    uint8_t raw[MN_XFER_CHUNK + 4];
    size_t  rn = 0;
    if (mbedtls_base64_decode(raw, sizeof(raw), &rn,
                              (const uint8_t *)b64, strlen(b64)) != 0)
        return -2;

    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (!s_tx.active)               { xSemaphoreGive(s_mx); return -1; }
    uint16_t n = tx_window_n();
    if (s_tx.fill >= n)             { xSemaphoreGive(s_mx); return -3; }
    uint16_t idx  = (uint16_t)(s_tx.base + s_tx.fill);
    uint16_t want = tx_chunk_len(idx);
    if (rn != want)                 { xSemaphoreGive(s_mx); return -5; }
    memcpy(s_tx.buf[s_tx.fill], raw, rn);
    s_tx.fill++;
    int filled = s_tx.fill;
    xSemaphoreGive(s_mx);
    return filled;
}

int mn_xfer_abort(void) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int rc = -1;
    if (s_tx.active) {
        send_abort(s_tx.peer, s_tx.xid, 0);
        mn_emit_event("!XFER_FAIL %04x aborted", s_tx.xid);
        s_tx.active = false;
        rc = 0;
    } else if (s_rx.active) {
        send_abort(s_rx.src, s_rx.xid, 0);
        mn_emit_event("!XFER_FAIL %04x aborted", s_rx.xid);
        rx_close();
        rc = 0;
    }
    xSemaphoreGive(s_mx);
    return rc;
}

void mn_xfer_status_print(void) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_tx.active) {
        int acked = 0;
        for (int i = 0; i < tx_window_n(); i++)
            if (s_tx.acked & (1ULL << i)) acked++;
        mn_emit_event("# xfer tx xid=%04x peer=%s base=%u/%u fill=%u acked=%d %s",
                      s_tx.xid, s_tx.peer, s_tx.base, s_tx.total_chunks,
                      s_tx.fill, acked, s_tx.accepted ? "accepted" : "init-wait");
    } else mn_emit_event("# xfer tx idle");
    if (s_rx.active) {
        mn_emit_event("# xfer rx xid=%04x from=%02x%02x%02x%02x %u/%u chunks",
                      s_rx.xid, s_rx.peer_id[0], s_rx.peer_id[1],
                      s_rx.peer_id[2], s_rx.peer_id[3],
                      s_rx.got, s_rx.total_chunks);
    } else mn_emit_event("# xfer rx idle");
    xSemaphoreGive(s_mx);
}

/* ---- sender: advance / complete (mutex held) ---- */

static void tx_advance(void) {
    uint16_t n = tx_window_n();
    if ((uint32_t)s_tx.base + n >= s_tx.total_chunks) {
        mn_emit_event("!XFER_SENT %04x len=%lu", s_tx.xid,
                      (unsigned long)s_tx.total_len);
        s_tx.active = false;
        return;
    }
    s_tx.base  = (uint16_t)(s_tx.base + n);
    s_tx.fill  = 0;
    s_tx.acked = 0;
    s_tx.sent  = 0;
    s_tx.rounds = 0;
    mn_emit_event("!XFER_NEXT %04x %u", s_tx.xid, s_tx.base);
}

/* ---- mesh side (pump task) ---- */

static void rx_emit_chunk(uint16_t idx, const uint8_t *data, size_t len) {
    if (!mn_xfer_ec_enabled()) return;
    /* "!XFER " + 8 id + " " + 4 xid + " idx/total " + b64(336)=448 ≈ 490 */
    static char line[512];                       /* pump task only */
    int n = snprintf(line, sizeof(line), "!XFER %02x%02x%02x%02x %04x %u/%u ",
                     s_rx.peer_id[0], s_rx.peer_id[1], s_rx.peer_id[2],
                     s_rx.peer_id[3], s_rx.xid, idx, s_rx.total_chunks);
    size_t bn = 0;
    if (mbedtls_base64_encode((uint8_t *)line + n, sizeof(line) - n - 1, &bn,
                              data, len) != 0) return;
    line[n + bn] = '\0';
    mn_write_line(line);
}

static void on_init(const uint8_t sender_id[4], const uint8_t *pl, size_t len,
                    const char *src) {
    if (len < 12) return;
    uint16_t xid    = get_u16(pl + 1);
    uint32_t tlen   = get_u32(pl + 3);
    uint16_t chunks = get_u16(pl + 7);
    uint16_t clen   = get_u16(pl + 9);
    uint8_t  mlen   = pl[11];
    if (len < (size_t)12 + mlen) return;

    if (s_rx.active) {
        if (s_rx.xid == xid && !memcmp(s_rx.peer_id, sender_id, 4)) {
            rx_send_status(XC_PROGRESS);         /* dup INIT: our accept lost */
        } else {
            send_status(src, xid, 0, 0, XC_BUSY);
        }
        return;
    }
    /* chunk_len bound keeps the !XFER event line inside the 512-char cap */
    if (tlen == 0 || clen == 0 || clen > MN_XFER_CHUNK ||
        chunks == 0 || chunks > MN_XFER_MAX_CHUNKS ||
        (uint32_t)(chunks - 1) * clen >= tlen || (uint32_t)chunks * clen < tlen) {
        send_status(src, xid, 0, 0, XC_REFUSED);
        return;
    }

    memset(&s_rx, 0, sizeof(s_rx));
    s_rx.active       = true;
    s_rx.xid          = xid;
    s_rx.total_len    = tlen;
    s_rx.total_chunks = chunks;
    s_rx.chunk_len    = clen;
    s_rx.last_rx_us   = esp_timer_get_time();
    memcpy(s_rx.peer_id, sender_id, 4);
    strlcpy(s_rx.src, src, sizeof(s_rx.src));

    char meta[X_META_MAX + 1];
    memcpy(meta, pl + 12, mlen);
    meta[mlen] = '\0';
    mn_emit_event("!XFER_BEGIN %02x%02x%02x%02x %04x %lu %u %u %s",
                  sender_id[0], sender_id[1], sender_id[2], sender_id[3],
                  xid, (unsigned long)tlen, chunks, clen,
                  mlen ? meta : "-");
    rx_send_status(XC_PROGRESS);                 /* the accept */
    tick_ensure_running();
}

static void on_data(const uint8_t sender_id[4], const uint8_t *pl, size_t len,
                    const char *src) {
    (void)src;
    if (len < 5 || !s_rx.active) return;
    uint16_t xid = get_u16(pl + 1);
    uint16_t idx = get_u16(pl + 3);
    if (xid != s_rx.xid || memcmp(sender_id, s_rx.peer_id, 4)) return;
    if (idx >= s_rx.total_chunks) return;

    uint16_t want = (idx == s_rx.total_chunks - 1)
                        ? (uint16_t)(s_rx.total_len - (uint32_t)idx * s_rx.chunk_len)
                        : s_rx.chunk_len;
    if (len - 5 != want) return;

    s_rx.last_rx_us = esp_timer_get_time();
    if (s_rx.bitmap[idx >> 3] & (1u << (idx & 7))) return;    /* dup chunk */
    s_rx.bitmap[idx >> 3] |= (1u << (idx & 7));
    s_rx.got++;

    rx_emit_chunk(idx, pl + 5, want);

    if (s_rx.got == s_rx.total_chunks) {
        mn_emit_event("!XFER_DONE %02x%02x%02x%02x %04x len=%lu",
                      s_rx.peer_id[0], s_rx.peer_id[1], s_rx.peer_id[2],
                      s_rx.peer_id[3], s_rx.xid, (unsigned long)s_rx.total_len);
        send_status(s_rx.src, s_rx.xid, s_rx.total_chunks, 0, XC_COMPLETE);
        rx_close();
        return;
    }
    /* window rolled over: first-missing moved into a later window → report */
    uint16_t fm   = rx_first_missing();
    uint16_t base = (uint16_t)(fm - (fm % MN_XFER_WINDOW));
    if (base != s_rx.report_base) rx_send_status(XC_PROGRESS);
}

static void on_status(const uint8_t *pl, size_t len) {
    if (len < 14 || !s_tx.active) return;
    if (get_u16(pl + 1) != s_tx.xid) return;
    uint16_t base = get_u16(pl + 3);
    uint64_t bm   = 0;
    for (int i = 0; i < 8; i++) bm |= ((uint64_t)pl[5 + i]) << (8 * i);
    uint8_t code = pl[13];

    s_tx.last_status_us = esp_timer_get_time();
    s_tx.rounds         = 0;
    s_tx.accepted       = true;

    if (code == XC_BUSY)    { tx_fail("busy");    return; }
    if (code == XC_REFUSED) { tx_fail("refused"); return; }
    if (code == XC_COMPLETE) {
        mn_emit_event("!XFER_SENT %04x len=%lu", s_tx.xid,
                      (unsigned long)s_tx.total_len);
        s_tx.active = false;
        return;
    }
    if (base > s_tx.base) { tx_advance(); return; }   /* window fully there */
    if (base < s_tx.base) return;                     /* stale */

    s_tx.acked |= bm;
    s_tx.sent  &= bm;              /* holes lose their sent bit → resent */
    uint16_t n = tx_window_n();
    uint64_t all = (n >= 64) ? ~0ULL : ((1ULL << n) - 1);
    if (s_tx.fill == n && (s_tx.acked & all) == all) tx_advance();
}

static void on_abort(const uint8_t *pl, size_t len) {
    if (len < 3) return;
    uint16_t xid = get_u16(pl + 1);
    if (s_tx.active && xid == s_tx.xid) tx_fail("peer-abort");
    if (s_rx.active && xid == s_rx.xid) rx_fail("peer-abort");
}

void mn_xfer_on_rx(const uint8_t sender_id[4], const uint8_t *pl, size_t len,
                   const char *src_ipv6) {
    if (len < 1 || !s_mx) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    switch (pl[0]) {
    case X_INIT:   on_init(sender_id, pl, len, src_ipv6); break;
    case X_DATA:   on_data(sender_id, pl, len, src_ipv6); break;
    case X_STATUS: on_status(pl, len);                    break;
    case X_ABORT:  on_abort(pl, len);                     break;
    default: break;
    }
    xSemaphoreGive(s_mx);
}

void mn_xfer_tick(void) {
    if (!s_mx) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int64_t now = esp_timer_get_time();

    if (s_tx.active) {
        if (!s_tx.accepted) {
            if (now - s_tx.last_init_us >= X_INIT_RETRY_US) {
                if (s_tx.init_tries >= X_INIT_TRIES_MAX) {
                    tx_fail("timeout");
                } else {
                    send_init();
                    s_tx.init_tries++;
                    s_tx.last_init_us = now;
                }
            }
        } else {
            /* resend round: receiver quiet too long with the window pending */
            uint16_t n = tx_window_n();
            uint64_t all = (n >= 64) ? ~0ULL : ((1ULL << n) - 1);
            bool pending = s_tx.fill > 0 &&
                           (s_tx.acked & all) != all &&
                           (s_tx.sent | s_tx.acked) ==
                               ((s_tx.fill >= 64) ? ~0ULL : ((1ULL << s_tx.fill) - 1));
            if (pending && now - s_tx.last_status_us >= X_STATUS_WAIT_US) {
                if (s_tx.rounds >= X_ROUNDS_MAX) {
                    tx_fail("timeout");
                } else {
                    s_tx.rounds++;
                    s_tx.sent &= s_tx.acked;      /* resend everything unacked */
                    s_tx.last_status_us = now;    /* one round per wait period */
                }
            }
            /* pace out unsent buffered chunks */
            if (s_tx.active) {
                int sent = 0;
                for (uint16_t i = 0; i < s_tx.fill && sent < X_CHUNKS_PER_TICK; i++) {
                    if ((s_tx.acked | s_tx.sent) & (1ULL << i)) continue;
                    uint16_t idx  = (uint16_t)(s_tx.base + i);
                    uint16_t clen = tx_chunk_len(idx);
                    uint8_t  pl[5 + MN_XFER_CHUNK];
                    pl[0] = X_DATA;
                    put_u16(pl + 1, s_tx.xid);
                    put_u16(pl + 3, idx);
                    memcpy(pl + 5, s_tx.buf[i], clen);
                    if (mn_xfer_send_frame(pl, 5 + clen, s_tx.peer) != 0)
                        break;                    /* backpressure: next tick */
                    s_tx.sent |= (1ULL << i);
                    sent++;
                }
            }
        }
    }

    if (s_rx.active) {
        if (now - s_rx.last_rx_us >= X_IDLE_ABORT_US) {
            rx_fail("timeout");
        } else if (now - s_rx.last_rx_us >= X_RX_GAP_US &&
                   now - s_rx.last_status_us >= X_RX_GAP_US &&
                   s_rx.got > 0) {
            rx_send_status(XC_PROGRESS);          /* NACK the holes */
        }
    }

    if (!s_tx.active && !s_rx.active && s_tick) xTimerStop(s_tick, 0);
    xSemaphoreGive(s_mx);
}
