/*
 * transport_relay — the framed-message transport (plan section 5.1, right-hand
 * branch). One request at a time: the supervisor already serialises check-ins,
 * and a relay that interleaved two requests over one BLE link would need a
 * per-request reassembly buffer the device does not have RAM for.
 *
 * Shape of a request, end to end:
 *
 *   supervisor ──request()──> build "{json}\n{body}" ──chunk──> pipe->write() ×N
 *                                                                     │
 *                                              (proxy performs the HTTP call)
 *                                                                     │
 *   supervisor <──status/body── reassemble into caller's buffer <─relay_rx() ×N
 *
 * Streaming (relay_request_stream, #101; brought back from the RobotARme
 * Mobile OTA fork, which measured it): a body goes to a sink frame by frame,
 * so ota_verify can fetch a release in ranged chunks and a lost frame costs
 * one chunk instead of the whole fetch. A stream is judged by silence, not by
 * its total time.
 *
 * The response is written STRAIGHT INTO THE CALLER'S BUFFER as frames arrive:
 * a check-in answer fits in 2 KB, a bundle in 64 KB, and both buffers already
 * exist in the supervisor. Overflow is flagged, not truncated-and-parsed, for
 * the same reason transport_ip returns -2: a clipped body that parses as
 * "noop" is the most dangerous possible misreading.
 */
#include <string.h>
#include <stdio.h>
#include "magnet_relay.h"
#include "relay_frame.h"
#include "magnet_cfg.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "transport_relay";

/* How long to wait for the proxy's complete answer. A 64 KB bundle at the
 * iOS MTU (185 B → ~370 frames) on a slow phone is a few seconds; the proxy's
 * own HTTP timeout is 15 s. 45 s covers both with margin, and a device that
 * waited longer would only be delaying its own "unreachable". */
#define RELAY_RESPONSE_TIMEOUT_MS 45000

/* The wait scales with what the caller is prepared to receive: 45 s plus 1 ms
 * per 4 bytes, so a slow link (~4 KB/s over BLE before #100) still finishes a
 * large body. A check-in's 2 KB buffer still waits ~45 s. */
static uint32_t response_timeout_ms(size_t cap) {
    return RELAY_RESPONSE_TIMEOUT_MS + (uint32_t)(cap / 4);
}

/* The largest frame we will ever build. BLE tops out at ATT MTU 517 - 3;
 * a serial pipe may claim more but gains nothing from it, and this buffer
 * lives in .bss for the life of the device. */
#define TX_FRAME_MAX 520

/* Streaming: the first frame may wait for the proxy's whole HTTP fetch; after
 * that frames come every few to a hundred ms. */
#define STREAM_FIRST_MS 30000
#define STREAM_IDLE_MS  4000

#define MAX_PIPES 2
static const relay_pipe_t *s_pipes[MAX_PIPES];
static int s_npipes;

static relay_stats_t s_stats;

/* The one in-flight request. Guarded by s_lock for the caller and read
 * lock-free by relay_rx(), which only ever touches it while s_waiting. */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_done;
static struct {
    bool     waiting;
    uint16_t seq;
    uint16_t next_idx;      /* frames must arrive in order */
    uint16_t total;
    char    *buf;           /* caller's response buffer (NULL when streaming) */
    relay_sink_fn sink;     /* streaming: gets each body slice of a 200 */
    void    *sink_ctx;
    bool     sink_failed;
    int64_t  last_rx_us;    /* streaming: when the last frame arrived */
    size_t   cap;
    size_t   len;
    bool     overflow;
    bool     error;         /* proxy sent RELAY_T_ERR */
    int      status;        /* HTTP status from the first RESP frame */
    bool     have_status;
} s_rx;

static uint16_t s_seq;

static esp_err_t relay_open(magnet_transport_t *t);

void relay_register_pipe(const relay_pipe_t *pipe) {
    if (!pipe || s_npipes >= MAX_PIPES) return;
    s_pipes[s_npipes++] = pipe;
    ESP_LOGI(TAG, "pipe registered: %s", pipe->name);
}

static const relay_pipe_t *attached_pipe(void) {
    for (int i = 0; i < s_npipes; ++i)
        if (s_pipes[i]->attached && s_pipes[i]->attached()) return s_pipes[i];
    return NULL;
}

bool relay_available(void) { return attached_pipe() != NULL; }
const char *relay_pipe_name(void) {
    const relay_pipe_t *p = attached_pipe();
    return p ? p->name : "none";
}
const relay_stats_t *relay_stats(void) { return &s_stats; }

/* ---- inbound ---------------------------------------------------------- */

static volatile bool s_in_rx;   /* relay_rx is running (see the stream give-up) */

static void relay_rx_inner(const uint8_t *frame, size_t len) {
    relay_frame_t f;
    s_stats.frames_in++;
    s_rx.last_rx_us = esp_timer_get_time();
    if (!relay_frame_decode(frame, len, &f)) {
        s_stats.bad_frames++;
        ESP_LOGW(TAG, "bad frame (%u bytes)", (unsigned)len);
        return;
    }
    if (!s_rx.waiting || f.seq != s_rx.seq) {
        /* Late answer to a request that already timed out, or noise. */
        ESP_LOGD(TAG, "frame seq %u ignored (waiting=%d cur=%u)", f.seq, s_rx.waiting, s_rx.seq);
        return;
    }
    if (f.type != RELAY_T_RESP && f.type != RELAY_T_ERR) return;
    if (f.idx != s_rx.next_idx) {
        /* Out of order on a reliable pipe means a frame was lost upstream
         * (the proxy dropped it, or a write failed). Nothing sane can be
         * rebuilt from here; fail the request rather than parse a body with
         * a hole in it. */
        ESP_LOGW(TAG, "frame idx %u, expected %u — aborting request", f.idx, s_rx.next_idx);
        s_rx.error = true;
        s_rx.waiting = false;
        xSemaphoreGive(s_done);
        return;
    }
    if (f.idx == 0) {
        s_rx.total = f.total;
        s_rx.error = (f.type == RELAY_T_ERR);
    }
    const uint8_t *p = f.payload;
    size_t n = f.len;
    if (!s_rx.have_status) {
        /* First two payload bytes of the message: the HTTP status. It may in
         * principle straddle frames; in practice frame 0 is never that small,
         * and we refuse rather than guess if it is. */
        if (n < 2) { s_rx.error = true; s_rx.waiting = false; xSemaphoreGive(s_done); return; }
        s_rx.status = (int)(p[0] | ((int)p[1] << 8));
        s_rx.have_status = true;
        p += 2; n -= 2;
    }
    if (n && s_rx.sink) {
        /* Streaming. Only a 200 or 206 body is the payload; an error body is
         * dropped. The sink runs here, in the pipe's RX path, and may take a
         * bounded time (a flash page write): the proxy waits for each write's
         * acknowledgement, so that time is the flow control. */
        if ((s_rx.status == 200 || s_rx.status == 206) && !s_rx.sink_failed && !s_rx.sink(s_rx.sink_ctx, p, n)) s_rx.sink_failed = true;
        s_rx.len += n;
        if (s_rx.sink_failed) {
            s_rx.waiting = false;
            xSemaphoreGive(s_done);
            return;
        }
    } else if (n) {
        size_t room = (s_rx.cap > s_rx.len + 1) ? (s_rx.cap - s_rx.len - 1) : 0;
        size_t take = n < room ? n : room;
        if (take < n) s_rx.overflow = true;
        if (take) { memcpy(s_rx.buf + s_rx.len, p, take); s_rx.len += take; s_rx.buf[s_rx.len] = '\0'; }
    }
    s_rx.next_idx++;
    if (s_rx.next_idx >= s_rx.total) {
        s_rx.waiting = false;
        xSemaphoreGive(s_done);
    }
}

void relay_rx(const uint8_t *frame, size_t len) {
    /* Set BEFORE relay_rx_inner reads s_rx.waiting: a requester that has just
     * given up (waiting = false) then waits for this to clear, so a sink call
     * already under way cannot touch the caller's state during its retry. */
    s_in_rx = true;
    relay_rx_inner(frame, len);
    s_in_rx = false;
}

/* ---- outbound --------------------------------------------------------- */

static int do_request(const char *method, const char *path, const char *body,
                      char *resp, size_t resp_cap, size_t *resp_len,
                      relay_sink_fn sink, void *sink_ctx, size_t expect_len) {
    const relay_pipe_t *pipe = attached_pipe();
    if (!pipe) return -1;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return -1;

    /* Request header line. Token and base URL ride along when present —
     * see magnet_relay.h for why the token crossing here is a decision, not
     * an oversight. The proxy supplies its own server when `s` is absent. */
    char token[CFG_MAX] = "", base[CFG_MAX] = "";
    cfg_get(CFG_DEV_TOKEN, token, sizeof token);
    cfg_get(CFG_SERVER_URL, base, sizeof base);
    char head[CFG_MAX * 2 + 320];
    int hl = snprintf(head, sizeof head, "{\"m\":\"%s\",\"p\":\"%s\"", method, path);
    if (token[0]) hl += snprintf(head + hl, sizeof head - hl, ",\"t\":\"%s\"", token);
    if (base[0])  hl += snprintf(head + hl, sizeof head - hl, ",\"s\":\"%s\"", base);
    hl += snprintf(head + hl, sizeof head - hl, "}\n");
    if (hl < 0 || (size_t)hl >= sizeof head) { xSemaphoreGive(s_lock); return -1; }

    size_t body_len = body ? strlen(body) : 0;
    size_t msg_len  = (size_t)hl + body_len;

    size_t max_frame = pipe->max_frame();
    if (max_frame > TX_FRAME_MAX) max_frame = TX_FRAME_MAX;
    if (max_frame < RELAY_MIN_FRAME) { ESP_LOGW(TAG, "pipe %s frame %u too small", pipe->name, (unsigned)max_frame); xSemaphoreGive(s_lock); return -1; }
    uint16_t total = relay_frames_for(msg_len, max_frame);
    if (!total) { xSemaphoreGive(s_lock); return -1; }
    size_t per = max_frame - RELAY_OVERHEAD;

    /* Arm the receiver BEFORE the first write: a fast proxy can answer a
     * one-frame request before this task returns from pipe->write(). */
    uint16_t seq = ++s_seq;
    memset(&s_rx, 0, sizeof s_rx);
    s_rx.seq = seq; s_rx.buf = resp; s_rx.cap = resp_cap; s_rx.waiting = true;
    s_rx.sink = sink; s_rx.sink_ctx = sink_ctx;
    if (resp && resp_cap) resp[0] = '\0';
    xSemaphoreTake(s_done, 0);          /* drain a stale give */
    s_stats.requests++;

    static uint8_t frame[TX_FRAME_MAX];
    size_t off = 0;
    int rc = 0;
    for (uint16_t i = 0; i < total && rc == 0; ++i) {
        size_t n = msg_len - off < per ? msg_len - off : per;
        /* The message is head followed by body, never copied whole: gather
         * the slice for this frame from whichever part(s) it spans. */
        uint8_t slice[TX_FRAME_MAX];
        for (size_t k = 0; k < n; ++k) {
            size_t pos = off + k;
            slice[k] = pos < (size_t)hl ? (uint8_t)head[pos] : (uint8_t)body[pos - hl];
        }
        size_t flen = relay_frame_encode(frame, sizeof frame, RELAY_T_REQ, seq, total, i, slice, (uint16_t)n);
        rc = flen ? pipe->write(frame, flen) : -1;
        if (rc == 0) s_stats.frames_out++;
        off += n;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "%s %s: pipe write failed (%d)", method, path, rc);
        s_rx.waiting = false;
        xSemaphoreGive(s_lock);
        return -1;
    }

    int status = -1;
    uint32_t wait_ms = response_timeout_ms(sink ? expect_len : resp_cap);
    bool done;
    if (!sink) {
        done = xSemaphoreTake(s_done, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
    } else {
        /* A stream is judged by silence, not by its total: once frames flow,
         * STREAM_IDLE_MS without one means a frame was lost at the tail (a
         * lost middle frame is caught at once by the index check) and the
         * caller retries the chunk, instead of waiting out the whole budget. */
        s_rx.last_rx_us = 0;
        int64_t start = esp_timer_get_time();
        done = false;
        for (;;) {
            if (xSemaphoreTake(s_done, pdMS_TO_TICKS(250)) == pdTRUE) { done = true; break; }
            int64_t now = esp_timer_get_time();
            if (s_rx.last_rx_us ? now - s_rx.last_rx_us > (int64_t)STREAM_IDLE_MS * 1000
                                : now - start > (int64_t)STREAM_FIRST_MS * 1000) break;
            if (now - start > (int64_t)wait_ms * 1000) break;
        }
    }
    if (!done) {
        s_rx.waiting = false;
        while (s_in_rx) vTaskDelay(1);
        s_stats.timeouts++;
        ESP_LOGW(TAG, "%s %s: no complete answer in %u ms", method, path, (unsigned)wait_ms);
    } else if (s_rx.error) {
        s_stats.responses++;
        ESP_LOGW(TAG, "%s %s: proxy error: %.120s", method, path, resp ? resp : "");
        status = -1;
    } else {
        s_stats.responses++;
        status = s_rx.status;
        if (status != 200) ESP_LOGW(TAG, "%s %s -> %d via %s", method, path, status, pipe->name);
        if (s_rx.overflow) { ESP_LOGW(TAG, "response truncated at %u bytes", (unsigned)resp_cap); status = -2; }
    }
    if (s_rx.sink_failed) { ESP_LOGW(TAG, "%s %s: sink refused the body at %u bytes", method, path, (unsigned)s_rx.len); status = -3; }
    if (resp_len) *resp_len = s_rx.len;
    xSemaphoreGive(s_lock);
    return status;
}

static int relay_request(magnet_transport_t *t,
                         const char *method, const char *path,
                         const char *body,
                         char *resp, size_t resp_cap, size_t *resp_len) {
    (void)t;
    return do_request(method, path, body, resp, resp_cap, resp_len, NULL, NULL, 0);
}

int relay_request_stream(const char *method, const char *path, const char *body,
                         relay_sink_fn sink, void *ctx, size_t expect_len, size_t *got) {
    if (!sink) return -1;
    if (!s_lock && relay_open(NULL) != ESP_OK) return -1;
    return do_request(method, path, body, NULL, 0, got, sink, ctx, expect_len);
}

static esp_err_t relay_open(magnet_transport_t *t) {
    (void)t;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    return (s_lock && s_done) ? ESP_OK : ESP_FAIL;
}
static void relay_close(magnet_transport_t *t) { (void)t; }

static magnet_transport_t s_relay = {
    .name = "relay",
    .open = relay_open,
    .request = relay_request,
    .close = relay_close,
    .ctx = NULL,
};

magnet_transport_t *transport_relay(void) { return &s_relay; }
