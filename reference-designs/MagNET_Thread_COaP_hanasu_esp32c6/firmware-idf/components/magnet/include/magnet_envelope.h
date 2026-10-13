/*
 * magnet_envelope.h — MagNET v2.1 message envelope (design proposal §11.1.4).
 *
 * E-Phase B scope: plaintext only. The 16-byte header is final; the AES-CCM
 * MIC (+8) and Ed25519 trailer (+64) land in E-Phase D — the pack/unpack API
 * already reserves the flag bits so D is additive, not a rewrite.
 */
#ifndef MAGNET_ENVELOPE_H
#define MAGNET_ENVELOPE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_ENV_VERSION   2      /* v2.1 wire format */
#define MN_ENV_HDR_LEN   16
#define MN_ENV_MAX_FRAME 512    /* cap accepted/produced envelope size (E-B) */

/* Message types (§4.4; Type 6 added rev 2.2 / E-H — see §11.8) */
enum {
    MN_T_CHAT     = 0,
    MN_T_M2M_CMD  = 1,
    MN_T_M2M_RESP = 2,
    MN_T_XFER     = 3,   /* reserved — superseded by Type 6 before it shipped */
    MN_T_ACK      = 4,
    MN_T_PING     = 5,
    MN_T_XFER6    = 6,   /* extended transfer: 16-bit chunk index in payload  */
};

/* Flags (§11.1.4) */
enum {
    MN_F_ENCRYPTED     = 1 << 0,
    MN_F_SIGNED        = 1 << 1,
    MN_F_ADMIN         = 1 << 2,
    MN_F_REQUIRES_ACK  = 1 << 3,
    MN_F_IS_FRAGMENT   = 1 << 4,
    MN_F_IS_FINAL_FRAG = 1 << 5,
    /* Machine-generated: this frame was emitted by an auto-responder, not by a
     * human at a keyboard. Auto-responders MUST NOT answer a frame carrying it
     * — that is what stops two bot-enabled nodes from volleying forever. The
     * bit is advisory and purely local to the sender's honesty; the per-peer
     * cooldown and reply budget in magnet_bot.c are the enforcing guards.
     * Older firmware ignores unknown flag bits, so this is wire-compatible. */
    MN_F_AUTOMATED     = 1 << 6,
};

typedef struct {
    uint8_t  version;       /* MN_ENV_VERSION */
    uint8_t  type;          /* MN_T_* */
    uint8_t  flags;         /* MN_F_* */
    uint8_t  sender_id[4];  /* SHA256(pk)[0:4] (E-B: derived from EUI-64) */
    uint32_t counter;       /* persistent monotonic per sender (E-B: per-boot) */
    uint8_t  epoch;         /* channel key epoch (E-B: 0) */
    uint16_t selector;      /* channel selector (E-B: default-channel constant) */
    uint8_t  frag_total;    /* 1..15 */
    uint8_t  frag_idx;      /* 0..14 */
    uint16_t msg_id;        /* fragment-group correlation */
    /* set by unpack, points into caller's buffer: */
    const uint8_t *payload;
    size_t         payload_len;
} mn_envelope_t;

/* Serialize header + payload into buf. Returns total bytes or <0 on error. */
int mn_env_pack(uint8_t *buf, size_t cap, const mn_envelope_t *e,
                const uint8_t *payload, size_t payload_len);

/* Parse buf into *e (payload points into buf). Returns 0 or <0 on error. */
int mn_env_unpack(mn_envelope_t *e, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MAGNET_ENVELOPE_H */
