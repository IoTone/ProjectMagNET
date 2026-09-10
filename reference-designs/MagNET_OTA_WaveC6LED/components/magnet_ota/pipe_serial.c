/*
 * pipe_serial — the USB-serial pipe for transport_relay (plan R4).
 *
 * The USB-serial-JTAG port already belongs to the console (console.c owns
 * the line loop), so the relay does not take the port over: it rides the
 * console as LINES with a distinctive prefix. Every other line is still the
 * console, so an operator can type `relay` mid-transfer and get an answer.
 *
 *   host → device   "!A\n"                 attach (and keep-alive; resend ≤ 20 s)
 *                   "!D\n"                 detach
 *                   "!R<base64 frame>\n"   one RESP/ERR frame
 *   device → host   "!R<base64 frame>\r\n" one REQ frame
 *
 * Base64 because the console's line reader accepts printable ASCII only and
 * a frame is binary. The console's LINE_MAX (200) bounds the frame: 196
 * base64 chars → 147 bytes → max_frame 144. Small, but USB is fast: a 64 KB
 * bundle is ~500 frames and lands in a second or two.
 *
 * "Attached" is a host that said !A within the last RELAY_SERIAL_STALE_MS.
 * A host that closed its tab without !D — the normal way a browser page
 * ends — must not leave the device routing check-ins into a dead port
 * forever, so attachment expires rather than being a latch.
 */
#include <string.h>
#include <stdio.h>
#include "magnet_relay.h"
#include "relay_frame.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "pipe_serial";

#define RELAY_SERIAL_LINE_MAX 200
#define RELAY_SERIAL_MAX_FRAME 144
#define RELAY_SERIAL_STALE_MS 45000

static void (*s_print)(const char *);
static int64_t s_last_host_us;      /* last !A / !R / !D from the host */
static bool    s_attached;

static bool serial_attached(void) {
    if (!s_attached) return false;
    if ((esp_timer_get_time() - s_last_host_us) / 1000 > RELAY_SERIAL_STALE_MS) {
        ESP_LOGI(TAG, "host silent for %d ms — detached", RELAY_SERIAL_STALE_MS);
        s_attached = false;
    }
    return s_attached;
}
static size_t serial_max_frame(void) { return RELAY_SERIAL_MAX_FRAME; }

static int serial_write(const uint8_t *frame, size_t len) {
    if (!s_print || !serial_attached()) return -1;
    char line[RELAY_SERIAL_LINE_MAX + 8];
    size_t olen = 0;
    line[0] = '!'; line[1] = 'R';
    if (mbedtls_base64_encode((unsigned char *)line + 2, sizeof line - 6, &olen, frame, len) != 0) return -1;
    line[2 + olen] = '\r'; line[3 + olen] = '\n'; line[4 + olen] = '\0';
    s_print(line);
    return 0;
}

static const relay_pipe_t s_pipe = {
    .name = "serial",
    .attached = serial_attached,
    .max_frame = serial_max_frame,
    .write = serial_write,
};

void pipe_serial_init(void (*print)(const char *)) {
    s_print = print;
    relay_register_pipe(&s_pipe);
}

/* The console hands over any line beginning with '!'. Returns true when the
 * line was ours (so the console neither evaluates it nor prompts). */
bool pipe_serial_line(const char *line) {
    if (!line || line[0] != '!') return false;
    s_last_host_us = esp_timer_get_time();
    switch (line[1]) {
    case 'A':
        if (!s_attached) ESP_LOGI(TAG, "host ATTACHED");
        s_attached = true;
        if (s_print) s_print("!K\r\n");           /* ack the keep-alive */
        return true;
    case 'D':
        if (s_attached) ESP_LOGI(TAG, "host detached");
        s_attached = false;
        return true;
    case 'R': {
        static uint8_t frame[RELAY_SERIAL_MAX_FRAME + 4];
        size_t olen = 0;
        const char *b64 = line + 2;
        if (mbedtls_base64_decode(frame, sizeof frame, &olen, (const unsigned char *)b64, strlen(b64)) != 0) {
            ESP_LOGW(TAG, "bad base64 line (%u chars)", (unsigned)strlen(b64));
            return true;
        }
        s_attached = true;                        /* a frame implies attachment */
        relay_rx(frame, olen);
        return true;
    }
    default:
        return false;                             /* not ours: Forth may want '!' */
    }
}

bool pipe_serial_attached(void) { return serial_attached(); }
