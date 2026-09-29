/*
 * magnet_crypto_psa.c — §11.1 crypto on the PSA Crypto API (Zephyr / MG24).
 *
 * Implements magnet_crypto.h exactly as firmware-idf's magnet_crypto.c does,
 * because Zephyr main ships mbedTLS 4 / TF-PSA-Crypto, where the legacy
 * ccm/hkdf/ecdsa/ecp headers no longer exist. On the MG24, PSA calls are
 * dispatched to the Secure Engine by hal_silabs' driver wrappers.
 *
 * Interop is the whole point: every derivation must be byte-identical to the
 * C6. Regression anchors (same as IDF): the well-known "magnet" channel must
 * derive selector 82f7, and deterministic ECDSA (RFC 6979) must produce the
 * same signature for the same key + message on both chips.
 */
#include "magnet_crypto.h"

#include <stdio.h>
#include <string.h>
#include <psa/crypto.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>          /* legacy HMAC used by the bundle engine (KAT) */
#include "esp_rom_crc.h"

#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const uint8_t CHAN_SALT[] = "MagNET/v2.1/chan-salt";

static bool crypto_up(void) {
    static bool up;
    if (!up) up = (psa_crypto_init() == PSA_SUCCESS);
    return up;
}

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

/* HKDF-SHA256. A NULL/empty salt is omitted, which RFC 5869 defines as
 * HashLen zeros — the same thing mbedtls_hkdf(NULL, 0, …) computes. */
static int hkdf(const uint8_t *salt, size_t salt_len,
                const uint8_t *ikm, size_t ikm_len,
                const uint8_t *info, size_t info_len,
                uint8_t *out, size_t out_len) {
    if (!crypto_up()) return -1;
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS && salt_len)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm, ikm_len);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_output_bytes(&op, out, out_len);
    psa_key_derivation_abort(&op);
    return st == PSA_SUCCESS ? 0 : -1;
}

static int hkdf32(const uint8_t *ikm, size_t ikm_len, const char *info,
                  uint8_t *out, size_t out_len) {
    return hkdf(CHAN_SALT, sizeof(CHAN_SALT) - 1, ikm, ikm_len,
                (const uint8_t *)info, strlen(info), out, out_len);
}

/* ---- software SHA-256 compression, for the PBKDF2 inner loop only ----
 * Every PSA call on the MG24 is a Secure Engine mailbox round trip (~320 us
 * per HMAC), which made the 100k-iteration Path B stretch take ~32 s (C6:
 * ~1 s). With the HMAC ipad/opad blocks pre-hashed once, each iteration is
 * exactly two compressions of one block each, done here in the CPU. The
 * power-on KAT cross-checks this path against PSA's one-shot PBKDF2. */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};
static const uint32_t SHA256_IV[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_compress(uint32_t st[8], const uint8_t blk[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[4*i] << 24) | ((uint32_t)blk[4*i+1] << 16) |
               ((uint32_t)blk[4*i+2] << 8) | blk[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3]; e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

/* HMAC(key, m) for a 32-byte m, given the pre-hashed ipad/opad states:
 * both the inner (64+32 bytes) and outer (64+32 bytes) messages finish in a
 * single padded block, whose length field is (64+32)*8 = 768 bits. */
static void hmac32_fast(const uint32_t ist[8], const uint32_t ost[8],
                        const uint8_t m[32], uint8_t out[32]) {
    uint8_t blk[64] = {0};
    uint32_t st[8];
    memcpy(blk, m, 32);
    blk[32] = 0x80; blk[62] = 0x03; blk[63] = 0x00;             /* 768 bits */
    memcpy(st, ist, sizeof(st));
    sha256_compress(st, blk);
    for (int i = 0; i < 8; i++) {                                 /* inner digest */
        blk[4*i] = st[i] >> 24; blk[4*i+1] = st[i] >> 16; blk[4*i+2] = st[i] >> 8; blk[4*i+3] = st[i];
    }
    memcpy(st, ost, sizeof(st));
    sha256_compress(st, blk);                                     /* same padding */
    for (int i = 0; i < 8; i++) {
        out[4*i] = st[i] >> 24; out[4*i+1] = st[i] >> 16; out[4*i+2] = st[i] >> 8; out[4*i+3] = st[i];
    }
}

/* PBKDF2-HMAC-SHA256, one 32-byte block, yielding — see the IDF file for
 * why it is hand-rolled (watchdog on the single-core C6). Zephyr would not
 * starve IDLE the same way, but keeping the identical loop keeps the two
 * implementations obviously equivalent. U_1 goes through PSA; U_2..U_c use
 * the software compression above. */
static int pbkdf2_yielding(const uint8_t *pw, size_t pw_len,
                           const uint8_t *salt, size_t salt_len,
                           uint32_t iters, uint8_t out32[32]) {
    if (!crypto_up() || iters == 0) return -1;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t key;
    if (psa_import_key(&a, pw, pw_len, &key) != PSA_SUCCESS) return -1;

    uint8_t first[64];
    uint8_t u[32], t[32];
    size_t olen;
    int rc = -1;
    if (salt_len > sizeof(first) - 4) goto done;
    memcpy(first, salt, salt_len);
    first[salt_len] = 0; first[salt_len + 1] = 0;
    first[salt_len + 2] = 0; first[salt_len + 3] = 1;      /* INT_32_BE(1) */
    if (psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), first, salt_len + 4,
                        u, sizeof(u), &olen) != PSA_SUCCESS) goto done;
    memcpy(t, u, sizeof(t));

    /* HMAC key block: the password, hashed first if it exceeds 64 bytes */
    uint8_t kb[64] = {0}, pad[64];
    uint32_t ist[8], ost[8];
    if (pw_len > 64) {
        if (psa_hash_compute(PSA_ALG_SHA_256, pw, pw_len, kb, 32, &olen) != PSA_SUCCESS) goto done;
    } else {
        memcpy(kb, pw, pw_len);
    }
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x36;
    memcpy(ist, SHA256_IV, sizeof(ist)); sha256_compress(ist, pad);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x5c;
    memcpy(ost, SHA256_IV, sizeof(ost)); sha256_compress(ost, pad);
    wipe(kb, sizeof(kb)); wipe(pad, sizeof(pad));

    for (uint32_t i = 1; i < iters; i++) {
        uint8_t next[32];
        hmac32_fast(ist, ost, u, next);
        memcpy(u, next, sizeof(u));
        for (size_t j = 0; j < sizeof(t); j++) t[j] ^= u[j];
        if (i % MN_PBKDF2_YIELD_EVERY == 0) vTaskDelay(1);
    }
    wipe(ist, sizeof(ist)); wipe(ost, sizeof(ost));
    memcpy(out32, t, sizeof(t));
    rc = 0;
done:
    psa_destroy_key(key);
    wipe(u, sizeof(u));
    wipe(t, sizeof(t));
    return rc;
}

void mn_root_expand(mn_channel_t *ch) {
    uint8_t sel[2], base[32];
    hkdf32(ch->root, 32, "selector", sel, 2);
    ch->selector = (uint16_t)((sel[0] << 8) | sel[1]);
    hkdf32(ch->root, 32, "mcast", ch->mcast_suffix, 4);
    hkdf32(ch->root, 32, "chan-base", base, 32);
    uint8_t info[8] = { 'e','p','o','c','h', 0, 0, 0 };
    hkdf(NULL, 0, base, sizeof(base), info, sizeof(info),
         ch->epoch_key, sizeof(ch->epoch_key));
    wipe(base, sizeof(base));
}

static int count_words(const char *s, size_t len) {
    int words = 0;
    bool in = false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ' ') in = false;
        else if (!in) { in = true; words++; }
    }
    return words;
}

int mn_cred_derive(const char *cred, size_t cred_len, mn_channel_t *out) {
    if (!cred || cred_len < 4 || cred_len > 512) return -1;
    memset(out, 0, sizeof(*out));

    if (cred_len > 3 && !strncmp(cred, "qr:", 3)) {          /* Path A */
        size_t olen = 0;
        uint8_t raw[64];
        if (mbedtls_base64_decode(raw, sizeof(raw), &olen,
                                  (const uint8_t *)cred + 3, cred_len - 3) != 0
            || olen != 32) return -2;
        memcpy(out->root, raw, 32);
        out->cred_path = 'A';
        strlcpy(out->name, "qr-channel", sizeof(out->name));
    } else if (count_words(cred, cred_len) >= 12) {          /* Path C */
        if (hkdf32((const uint8_t *)cred, cred_len, "root", out->root, 32) != 0)
            return -3;
        out->cred_path = 'C';
        size_t n = 0;
        while (n < cred_len && cred[n] != ' ' && n < sizeof(out->name) - 1) n++;
        memcpy(out->name, cred, n);
        out->name[n] = '\0';
    } else {                                                 /* Path B */
        uint8_t salt[32];
        hkdf32((const uint8_t *)cred, cred_len, "chan-salt-ikm", salt, 32);
        if (pbkdf2_yielding((const uint8_t *)cred, cred_len,
                            salt, sizeof(salt),
                            MN_PBKDF2_ITERS, out->root) != 0) return -4;
        out->cred_path = 'B';
        size_t n = cred_len < sizeof(out->name) - 1 ? cred_len : sizeof(out->name) - 1;
        memcpy(out->name, cred, n);
        out->name[n] = '\0';
    }
    mn_root_expand(out);
    out->set = true;
    return 0;
}

void mn_chan_epoch_key(mn_channel_t *ch, uint8_t e) {
    uint8_t base[32];
    hkdf32(ch->root, 32, "chan-base", base, 32);
    uint8_t info[8] = { 'e','p','o','c','h', 0, 0, e };
    hkdf(NULL, 0, base, sizeof(base), info, sizeof(info),
         ch->epoch_key, sizeof(ch->epoch_key));
    wipe(base, sizeof(base));
}

void mn_nonce_build(uint8_t nonce13[13], const uint8_t device_id[4],
                    uint32_t counter, uint8_t epoch, uint16_t selector) {
    memcpy(nonce13, device_id, 4);
    nonce13[4] = (uint8_t)(counter >> 24);
    nonce13[5] = (uint8_t)(counter >> 16);
    nonce13[6] = (uint8_t)(counter >> 8);
    nonce13[7] = (uint8_t)counter;
    nonce13[8] = epoch;
    nonce13[9]  = (uint8_t)(selector >> 8);
    nonce13[10] = (uint8_t)selector;
    nonce13[11] = 0;
    nonce13[12] = 0;
}

/* ---- AES-128-CCM, 8-byte tag. PSA wants ct‖tag contiguous; MagNET keeps
 * the MIC separate, so both directions go through a scratch buffer. ---- */
#define CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 8)
#define CCM_MAX 600                          /* > 512-byte envelope cap */

static int ccm_key(const uint8_t key[16], psa_key_id_t *id) {
    if (!crypto_up()) return -1;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, CCM_ALG);
    return psa_import_key(&a, key, 16, id) == PSA_SUCCESS ? 0 : -1;
}

int mn_aead_encrypt(const uint8_t key[16], const uint8_t nonce13[13],
                    const uint8_t aad16[16], const uint8_t *pt, size_t len,
                    uint8_t *ct, uint8_t mic[8]) {
    if (len + 8 > CCM_MAX) return -1;
    psa_key_id_t id;
    if (ccm_key(key, &id) != 0) return -1;
    uint8_t buf[CCM_MAX];
    size_t olen = 0;
    psa_status_t st = psa_aead_encrypt(id, CCM_ALG, nonce13, 13, aad16, 16,
                                       pt, len, buf, len + 8, &olen);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || olen != len + 8) return -1;
    memcpy(ct, buf, len);
    memcpy(mic, buf + len, 8);
    wipe(buf, olen);
    return 0;
}

int mn_aead_decrypt(const uint8_t key[16], const uint8_t nonce13[13],
                    const uint8_t aad16[16], const uint8_t *ct, size_t len,
                    uint8_t *pt, const uint8_t mic[8]) {
    if (len + 8 > CCM_MAX) return -1;
    psa_key_id_t id;
    if (ccm_key(key, &id) != 0) return -1;
    uint8_t in[CCM_MAX];
    memcpy(in, ct, len);
    memcpy(in + len, mic, 8);
    size_t olen = 0;
    psa_status_t st = psa_aead_decrypt(id, CCM_ALG, nonce13, 13, aad16, 16,
                                       in, len + 8, pt, len, &olen);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS && olen == len) ? 0 : -1;
}

/* ================= device identity: deterministic ECDSA P-256 ============= */
/* The private scalar is stored raw in NVS "magnet"/"idkey", the same format
 * as the IDF build, so the key survives a future move to PSA ITS without a
 * format change. The key object itself is volatile (re-imported at boot). */

#define ECC_ATTRS_TYPE PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)
#define SIGN_ALG       PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256)

static psa_key_id_t s_id_key;
static bool s_ident_ready = false;

static int import_scalar(const uint8_t d[32], psa_key_id_t *id) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, ECC_ATTRS_TYPE);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&a, SIGN_ALG);
    return psa_import_key(&a, d, 32, id) == PSA_SUCCESS ? 0 : -1;
}

static int sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    size_t olen;
    return psa_hash_compute(PSA_ALG_SHA_256, msg, len, out, 32, &olen) == PSA_SUCCESS
           ? 0 : -1;
}

int mn_ident_load_or_gen(uint8_t pub65[65], uint8_t device_id[4]) {
    if (!crypto_up()) return -1;

    uint8_t dbuf[32];
    size_t blen = sizeof(dbuf);
    nvs_handle_t h;
    bool have = false;
    if (nvs_open("magnet", NVS_READONLY, &h) == ESP_OK) {
        have = (nvs_get_blob(h, "idkey", dbuf, &blen) == ESP_OK && blen == 32);
        nvs_close(h);
    }
    if (have) {
        if (import_scalar(dbuf, &s_id_key) != 0) { wipe(dbuf, 32); return -2; }
    } else {
        /* A random 32-byte string is a valid P-256 scalar unless it is 0 or
         * ≥ n (probability ~2^-32); import rejects those, so just redraw. */
        int tries = 0;
        do {
            if (psa_generate_random(dbuf, 32) != PSA_SUCCESS) return -3;
        } while (import_scalar(dbuf, &s_id_key) != 0 && ++tries < 8);
        if (tries >= 8) { wipe(dbuf, 32); return -3; }
        if (nvs_open("magnet", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "idkey", dbuf, 32);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    wipe(dbuf, sizeof(dbuf));

    size_t olen = 0;
    if (psa_export_public_key(s_id_key, pub65, 65, &olen) != PSA_SUCCESS || olen != 65)
        return -4;
    uint8_t hash[32];
    if (sha256(pub65, 65, hash) != 0) return -4;
    memcpy(device_id, hash, 4);          /* §11.1.1: id = SHA256(pk)[0:4] */
    s_ident_ready = true;
    return 0;
}

int mn_sign(const uint8_t *msg, size_t len, uint8_t sig64[64]) {
    if (!s_ident_ready) return -1;
    uint8_t hash[32];
    if (sha256(msg, len, hash) != 0) return -2;
    size_t olen = 0;
    psa_status_t st = psa_sign_hash(s_id_key, SIGN_ALG, hash, 32, sig64, 64, &olen);
    return (st == PSA_SUCCESS && olen == 64) ? 0 : -2;
}

int mn_verify(const uint8_t pub65[65], const uint8_t *msg, size_t len,
              const uint8_t sig64[64]) {
    if (!crypto_up()) return -1;
    uint8_t hash[32];
    if (sha256(msg, len, hash) != 0) return -1;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_HASH);
    /* verify accepts any ECDSA flavour: RFC 6979 signatures are plain ECDSA */
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t id;
    if (psa_import_key(&a, pub65, 65, &id) != PSA_SUCCESS) return -1;
    psa_status_t st = psa_verify_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32, sig64, 64);
    psa_destroy_key(id);
    return st == PSA_SUCCESS ? 0 : -1;
}

/* ======================= power-on known-answer tests ======================
 * Proves the PSA port computes what the C6's mbedTLS computes, without
 * touching node state. Deterministic ECDSA has exactly one right answer per
 * (key, message), so the RFC 6979 vector pins signing byte-for-byte. */

static int hex_eq(const uint8_t *b, const char *hex, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1 || b[i] != (uint8_t)v) return 0;
    }
    return 1;
}

static void hex_in(uint8_t *b, const char *hex, size_t n) {
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); b[i] = (uint8_t)v; }
}

/* RFC 6979 A.2.5: P-256, SHA-256, message "sample" */
static int kat_rfc6979(void) {
    uint8_t d[32], sig[64], hash[32];
    hex_in(d, "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721", 32);
    psa_key_id_t id;
    if (import_scalar(d, &id) != 0) return -1;
    size_t olen = 0;
    int rc = -2;
    psa_status_t st = PSA_ERROR_GENERIC_ERROR;
    if (sha256((const uint8_t *)"sample", 6, hash) == 0)
        st = psa_sign_hash(id, SIGN_ALG, hash, 32, sig, 64, &olen);
    if (st != PSA_SUCCESS) { psa_destroy_key(id); return (int)st; }   /* PSA code */
    if (olen == 64) {
        rc = (hex_eq(sig,      "EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716", 32) &&
              hex_eq(sig + 32, "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8", 32))
             ? 0 : -3;
    }
    uint8_t pub[65];
    if (rc == 0 && (psa_export_public_key(id, pub, 65, &olen) != PSA_SUCCESS ||
                    mn_verify(pub, (const uint8_t *)"sample", 6, sig) != 0)) rc = -4;
    psa_destroy_key(id);
    return rc;
}

#ifdef PSA_WANT_ALG_PBKDF2_HMAC
static int pbkdf2_psa(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                      uint32_t iters, uint8_t out32[32]) {
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iters);
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    if (st == PSA_SUCCESS) st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, pw, pw_len);
    if (st == PSA_SUCCESS) st = psa_key_derivation_output_bytes(&op, out32, 32);
    psa_key_derivation_abort(&op);
    return st == PSA_SUCCESS ? 0 : (int)st;
}
#endif

void mn_crypto_psa_kat(char *out, size_t cap) {
    if (!crypto_up()) { snprintf(out, cap, "# crypto-kat FAIL psa_crypto_init"); return; }
    int ecdsa = kat_rfc6979();
    /* bundle engine primitives: zlib CRC-32 check value, and RFC 4231 case 2
     * through the legacy mbedtls_md_hmac() the v1 (HMAC) bundle path calls */
    bool crc_ok = esp_rom_crc32_le(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u;
    uint8_t mac[32];
    const char *jd = "what do ya want for nothing?";
    bool hmac_ok = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                   (const uint8_t *)"Jefe", 4, (const uint8_t *)jd,
                                   strlen(jd), mac) == 0 &&
        hex_eq(mac, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);
    /* Path A credentials are "qr:<base64 of 32 bytes>" */
    uint8_t raw[64];
    size_t olen = 0;
    static const char b64[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";   /* 0x00..0x1f */
    int brc = mbedtls_base64_decode(raw, sizeof(raw), &olen, (const uint8_t *)b64, sizeof(b64) - 1);
    bool b64_ok = brc == 0 && olen == 32 && raw[0] == 0 && raw[31] == 0x1f;
    int n = snprintf(out, cap, "# crypto-kat rfc6979-p256=%s(%d) crc32=%s hmac-rfc4231=%s b64=%s(rc=%d,len=%u)",
                     ecdsa ? "FAIL" : "ok", ecdsa, crc_ok ? "ok" : "FAIL", hmac_ok ? "ok" : "FAIL",
                     b64_ok ? "ok" : "FAIL", brc, (unsigned)olen);
#ifdef PSA_WANT_ALG_PBKDF2_HMAC
    /* the loop and the one-shot must agree bit-for-bit (short run), then
     * time the one-shot at the real iteration count */
    static const uint8_t salt[32] = { 1, 2, 3 };
    uint8_t a[32], b[32];
    int la = pbkdf2_yielding((const uint8_t *)"magnet", 6, salt, 32, 1000, a);
    int lb = pbkdf2_psa((const uint8_t *)"magnet", 6, salt, 32, 1000, b);
    bool same = (la == 0 && lb == 0 && !memcmp(a, b, 32));
#ifdef MN_KAT_TIME_PBKDF2
    int64_t t0 = esp_timer_get_time();
    int lc = pbkdf2_yielding((const uint8_t *)"magnet", 6, salt, 32, MN_PBKDF2_ITERS, a);
    int64_t dt = (esp_timer_get_time() - t0) / 1000;
    snprintf(out + n, cap - n, " pbkdf2 fast-vs-psa=%s fast-%u-iters=%lldms(rc=%d)",
             same ? "match" : "DIFFER", (unsigned)MN_PBKDF2_ITERS, (long long)dt, lc);
#else
    snprintf(out + n, cap - n, " pbkdf2 fast-vs-psa=%s", same ? "match" : "DIFFER");
#endif
#else
    snprintf(out + n, cap - n, " pbkdf2-psa=not-built");
#endif
}
