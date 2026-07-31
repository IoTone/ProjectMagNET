/*
 * magnet_envelope.c — v2.1 envelope pack/unpack (§11.1.4). Plaintext (E-B).
 */
#include "magnet_envelope.h"

#include <string.h>

int mn_env_pack(uint8_t *buf, size_t cap, const mn_envelope_t *e,
                const uint8_t *payload, size_t payload_len)
{
    if (!buf || !e) return -1;
    if (payload_len > 0 && !payload) return -1;
    size_t total = MN_ENV_HDR_LEN + payload_len;
    if (total > cap || total > MN_ENV_MAX_FRAME) return -2;
    if (e->frag_total < 1 || e->frag_total > 15 || e->frag_idx > 14) return -3;

    buf[0]  = (uint8_t)((e->version & 0x0F) << 4) | (e->type & 0x0F);
    buf[1]  = e->flags;
    memcpy(&buf[2], e->sender_id, 4);
    buf[6]  = (uint8_t)(e->counter >> 24);
    buf[7]  = (uint8_t)(e->counter >> 16);
    buf[8]  = (uint8_t)(e->counter >> 8);
    buf[9]  = (uint8_t)(e->counter);
    buf[10] = e->epoch;
    buf[11] = (uint8_t)(e->selector >> 8);
    buf[12] = (uint8_t)(e->selector);
    buf[13] = (uint8_t)((e->frag_total & 0x0F) << 4) | (e->frag_idx & 0x0F);
    buf[14] = (uint8_t)(e->msg_id >> 8);
    buf[15] = (uint8_t)(e->msg_id);
    if (payload_len) memcpy(&buf[MN_ENV_HDR_LEN], payload, payload_len);
    return (int)total;
}

int mn_env_unpack(mn_envelope_t *e, const uint8_t *buf, size_t len)
{
    if (!e || !buf || len < MN_ENV_HDR_LEN || len > MN_ENV_MAX_FRAME) return -1;

    e->version = buf[0] >> 4;
    e->type    = buf[0] & 0x0F;
    if (e->version != MN_ENV_VERSION) return -2;
    e->flags   = buf[1];
    memcpy(e->sender_id, &buf[2], 4);
    e->counter = ((uint32_t)buf[6] << 24) | ((uint32_t)buf[7] << 16) |
                 ((uint32_t)buf[8] << 8)  |  (uint32_t)buf[9];
    e->epoch      = buf[10];
    e->selector   = (uint16_t)((buf[11] << 8) | buf[12]);
    e->frag_total = buf[13] >> 4;
    e->frag_idx   = buf[13] & 0x0F;
    if (e->frag_total < 1 || e->frag_idx >= e->frag_total) return -3;
    e->msg_id     = (uint16_t)((buf[14] << 8) | buf[15]);
    e->payload     = &buf[MN_ENV_HDR_LEN];
    e->payload_len = len - MN_ENV_HDR_LEN;
    return 0;
}
