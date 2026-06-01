/*
 * magnet_lineages — DNA key table + puzzle-response helper.
 *
 * See magnet_lineages.h for protocol intent. Each MAJOR firmware family has
 * its own 32-byte key; old keys remain in the table forever so a newer
 * ruler can still recognise older biologics.
 */

#include "magnet_lineages.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/md.h"

/* Lineage DNA keys.
 *
 * spore (0.x): generated 2026-04-26 with `python -c "import secrets;
 * print(secrets.token_bytes(32).hex())"`. Compiled-in for dev convenience.
 * Production deployments SHOULD regenerate and rotate via NVS, but rotating
 * a lineage key is rare — it's only the second authentication factor on top
 * of the hive secret.
 *
 * Do not reorder or delete entries: old biologics that pre-date a MAJOR
 * bump still authenticate via their original lineage's key.
 */
const magnet_lineage_t MAGNET_LINEAGES[] = {
    {
        .codename = "spore",
        .key = {
            0x00, 0x60, 0x1D, 0xA9, 0xCC, 0x21, 0xB7, 0x23,
            0x3E, 0xFD, 0x11, 0x6E, 0x41, 0xEC, 0xC9, 0x55,
            0x78, 0xDC, 0x5A, 0x59, 0xBE, 0x3F, 0xD4, 0xC3,
            0x4F, 0x9A, 0x40, 0xE7, 0x88, 0xA2, 0x9E, 0x5E,
        },
    },
    /* Future entries appended at MAJOR bumps:
     *   { "hyphae",    { ... 32 bytes ... } },     // gen 1.x
     *   { "mycelium",  { ... 32 bytes ... } },     // gen 2.x
     *   { "fruiting",  { ... 32 bytes ... } },     // gen 3.x
     *   { "sporocarp", { ... 32 bytes ... } },     // gen 4.x
     */
    { NULL, {0} },
};

const magnet_lineage_t *magnet_lineage_find(const char *codename) {
    if (!codename || !*codename) return NULL;
    for (const magnet_lineage_t *p = MAGNET_LINEAGES; p->codename; p++) {
        if (strcmp(p->codename, codename) == 0) return p;
    }
    return NULL;
}

int magnet_lineage_from_gen(const char *gen_str, char *out, size_t out_len) {
    if (!gen_str || !out || out_len < 2) return -1;
    const char *dash = strrchr(gen_str, '-');
    if (!dash || !dash[1]) return -1;
    size_t n = strlen(dash + 1);
    if (n + 1 > out_len) return -1;
    memcpy(out, dash + 1, n);
    out[n] = '\0';
    return 0;
}

static int hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t out[32]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    return mbedtls_md_hmac(info, key, key_len, data, data_len, out);
}

static const char HEX[] = "0123456789abcdef";

int magnet_lineage_compute_response(const uint8_t key[MAGNET_LINEAGE_KEY_BYTES],
                                    const char *puzzle,
                                    const char *node_id,
                                    int64_t ts,
                                    char *out_hex,
                                    size_t out_hex_len) {
    if (!key || !puzzle || !node_id || !out_hex || out_hex_len < 65) return -1;

    /* Concatenate puzzle || "|" || node_id || "|" || ts(decimal). The
     * separators are belt-and-suspenders — they prevent a clever choice of
     * fields from hashing the same byte sequence as a different (puzzle,
     * id, ts) triple. */
    char ts_buf[24];
    snprintf(ts_buf, sizeof(ts_buf), "%" PRId64, ts);

    size_t plen = strlen(puzzle);
    size_t nlen = strlen(node_id);
    size_t tlen = strlen(ts_buf);
    size_t total = plen + 1 + nlen + 1 + tlen;
    if (total > 512) return -1;

    uint8_t buf[512];
    size_t w = 0;
    memcpy(buf + w, puzzle, plen); w += plen;
    buf[w++] = '|';
    memcpy(buf + w, node_id, nlen); w += nlen;
    buf[w++] = '|';
    memcpy(buf + w, ts_buf, tlen); w += tlen;

    uint8_t mac[32];
    if (hmac_sha256(key, MAGNET_LINEAGE_KEY_BYTES, buf, w, mac) != 0) return -2;

    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = HEX[(mac[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = HEX[mac[i] & 0xF];
    }
    out_hex[64] = '\0';
    return 0;
}
