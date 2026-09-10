/*
 * relay_frame — the wire format shared by every relay pipe and every proxy.
 *
 * Plan section 5.2, verbatim:
 *
 *   <u8 version> <u8 type> <u16 seq> <u16 total_frames> <u16 frame_idx>
 *   <u16 payload_len> <payload...> <u16 crc16>
 *
 * Integers little-endian. CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over
 * HEADER AND PAYLOAD — the plan says payload; covering the header too costs
 * nothing and catches a corrupted seq or idx, which would otherwise be
 * accepted and silently misplace a chunk.
 *
 * Chunking is explicit rather than trusting the pipe's MTU: BLE's is
 * negotiated (~180–500 B practical), serial's is whatever the line reader
 * takes. The device does not care which it is on.
 *
 * Payloads (the message a frame sequence carries once reassembled):
 *
 *   REQ  (device → proxy)   one JSON line, '\n', then the raw body:
 *        {"m":"POST","p":"/api/devices/check-in","t":"dvc_...","s":"http://..."}\n{...}
 *        m = method, p = server-relative path (every URL the server hands a
 *        device is relative — signed download URLs included), t = bearer
 *        token when the device has one, s = base URL when the device has one
 *        (the proxy falls back to its own configured server). No base64, no
 *        escaping of the body: bodies are JSON or Forth source, and the split
 *        on the first newline is unambiguous because the JSON line has none.
 *
 *   RESP (proxy → device)   <u16 http_status> then the raw response body.
 *
 *   ERR  (proxy → device)   <u16 0> then a short reason. The transport turns
 *        this into a NEGATIVE return, keeping "the server said no" (RESP with
 *        a 4xx) distinct from "never reached the server" — the distinction
 *        magnet_transport.h insists on.
 *
 *   ACK  reserved. Both pipes are reliable and ordered underneath (L2CAP,
 *        USB), so per-frame acks would be ceremony. The type exists so a lossy
 *        pipe (LoRa, someday) can add them without a version bump.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_FRAME_VERSION 1
#define RELAY_HDR_LEN       10
#define RELAY_CRC_LEN       2
#define RELAY_OVERHEAD      (RELAY_HDR_LEN + RELAY_CRC_LEN)
/* Below this a frame carries almost nothing; a pipe reporting less is broken. */
#define RELAY_MIN_FRAME     (RELAY_OVERHEAD + 8)

enum {
    RELAY_T_REQ  = 1,
    RELAY_T_RESP = 2,
    RELAY_T_ACK  = 3,
    RELAY_T_ERR  = 4,
};

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;
    uint16_t total;
    uint16_t idx;
    uint16_t len;
    const uint8_t *payload;     /* points into the caller's buffer */
} relay_frame_t;

uint16_t relay_crc16(const uint8_t *data, size_t len);

/* Build one frame into OUT (cap >= RELAY_OVERHEAD + payload_len).
 * Returns the frame length, or 0 if it does not fit. */
size_t relay_frame_encode(uint8_t *out, size_t cap,
                          uint8_t type, uint16_t seq, uint16_t total, uint16_t idx,
                          const uint8_t *payload, uint16_t payload_len);

/* Parse and validate (version, length, CRC). False on anything wrong. */
bool relay_frame_decode(const uint8_t *buf, size_t len, relay_frame_t *out);

/* How many frames a message of MSG_LEN bytes needs at MAX_FRAME bytes per
 * frame. At least 1 — an empty message is still one (empty) frame. */
uint16_t relay_frames_for(size_t msg_len, size_t max_frame);

#ifdef __cplusplus
}
#endif
