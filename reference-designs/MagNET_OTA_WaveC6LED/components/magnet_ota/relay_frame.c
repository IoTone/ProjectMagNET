/*
 * relay_frame — codec. No allocation, no globals, no ESP-IDF dependency, so
 * it is the one file here that can be compiled and tested on a host as-is.
 */
#include <string.h>
#include "relay_frame.h"

uint16_t relay_crc16(const uint8_t *data, size_t len) {
    /* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.
     * Check value for "123456789" is 0x29B1 — asserted by the Dart and JS
     * tests too, so all three codecs prove they agree on the same constant. */
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p)   { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

size_t relay_frame_encode(uint8_t *out, size_t cap,
                          uint8_t type, uint16_t seq, uint16_t total, uint16_t idx,
                          const uint8_t *payload, uint16_t payload_len) {
    size_t need = RELAY_OVERHEAD + payload_len;
    if (!out || cap < need) return 0;
    out[0] = RELAY_FRAME_VERSION;
    out[1] = type;
    put16(out + 2, seq);
    put16(out + 4, total);
    put16(out + 6, idx);
    put16(out + 8, payload_len);
    if (payload_len) memcpy(out + RELAY_HDR_LEN, payload, payload_len);
    put16(out + RELAY_HDR_LEN + payload_len,
          relay_crc16(out, RELAY_HDR_LEN + payload_len));
    return need;
}

bool relay_frame_decode(const uint8_t *buf, size_t len, relay_frame_t *f) {
    if (!buf || !f || len < RELAY_OVERHEAD) return false;
    if (buf[0] != RELAY_FRAME_VERSION) return false;
    uint16_t plen = get16(buf + 8);
    if ((size_t)RELAY_OVERHEAD + plen != len) return false;
    if (relay_crc16(buf, RELAY_HDR_LEN + plen) != get16(buf + RELAY_HDR_LEN + plen)) return false;
    f->version = buf[0];
    f->type    = buf[1];
    f->seq     = get16(buf + 2);
    f->total   = get16(buf + 4);
    f->idx     = get16(buf + 6);
    f->len     = plen;
    f->payload = buf + RELAY_HDR_LEN;
    /* A frame past the end of its own sequence is corrupt even if the CRC
     * says otherwise (which it cannot, but belt and braces). */
    return f->total > 0 && f->idx < f->total;
}

uint16_t relay_frames_for(size_t msg_len, size_t max_frame) {
    if (max_frame <= RELAY_OVERHEAD) return 0;
    size_t per = max_frame - RELAY_OVERHEAD;
    size_t n = (msg_len + per - 1) / per;
    if (n == 0) n = 1;
    return n > 0xFFFF ? 0 : (uint16_t)n;
}
