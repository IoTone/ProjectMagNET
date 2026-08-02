/*
 * magnet_crypto.c — §11.1 credential derivation + AES-CCM AEAD (E-Phase D).
 */
#include "magnet_crypto.h"

#include <string.h>
#include "mbedtls/ccm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
/* Only for the cooperative yield inside the PBKDF2 loop — see pbkdf2_yielding. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const uint8_t CHAN_SALT[] = "MagNET/v2.1/chan-salt";

/* PBKDF2-HMAC-SHA256 producing exactly one 32-byte block, yielding as it goes.
 *
 * mbedtls_pkcs5_pbkdf2_hmac_ext() does all MN_PBKDF2_ITERS iterations without
 * ever returning, which on this single-core part starves IDLE for seconds and
 * trips the task watchdog mid-`CHANNEL SET`. Same arithmetic, just interrupted
 * often enough for the scheduler to breathe.
 *
 * dkLen == hLen == 32, so RFC 8018 collapses to a single block:
 *     DK = T_1 = U_1 XOR U_2 XOR … XOR U_c
 *     U_1 = HMAC(P, S ‖ INT_32_BE(1)),  U_i = HMAC(P, U_{i-1})
 * Byte-identical to the mbedtls call it replaces — the well-known "magnet"
 * channel must keep deriving selector 82f7, which is the regression test.
 */
static int pbkdf2_yielding(const uint8_t *pw, size_t pw_len,
                           const uint8_t *salt, size_t salt_len,
                           uint32_t iters, uint8_t out32[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const uint8_t idx_be[4] = { 0, 0, 0, 1 };
    mbedtls_md_context_t ctx;
    uint8_t u[32], t[32];
    int rc = -1;

    if (!md || iters == 0) return -1;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, 1) != 0) goto done;      /* 1 = HMAC */

    if (mbedtls_md_hmac_starts(&ctx, pw, pw_len) != 0 ||
        mbedtls_md_hmac_update(&ctx, salt, salt_len) != 0 ||
        mbedtls_md_hmac_update(&ctx, idx_be, sizeof(idx_be)) != 0 ||
        mbedtls_md_hmac_finish(&ctx, u) != 0) goto done;
    memcpy(t, u, sizeof(t));

    for (uint32_t i = 1; i < iters; i++) {
        if (mbedtls_md_hmac_reset(&ctx) != 0 ||             /* re-primes with pw */
            mbedtls_md_hmac_update(&ctx, u, sizeof(u)) != 0 ||
            mbedtls_md_hmac_finish(&ctx, u) != 0) goto done;
        for (size_t j = 0; j < sizeof(t); j++) t[j] ^= u[j];
        /* vTaskDelay, not taskYIELD: mn_link outranks IDLE, so only actually
         * blocking lets IDLE run and the watchdog get fed. */
        if (i % MN_PBKDF2_YIELD_EVERY == 0) vTaskDelay(1);
    }
    memcpy(out32, t, sizeof(t));
    rc = 0;
done:
    mbedtls_md_free(&ctx);
    mbedtls_platform_zeroize(u, sizeof(u));
    mbedtls_platform_zeroize(t, sizeof(t));
    return rc;
}

static int hkdf32(const uint8_t *ikm, size_t ikm_len, const char *info,
                  uint8_t *out, size_t out_len) {
    return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        CHAN_SALT, sizeof(CHAN_SALT) - 1,
                        ikm, ikm_len,
                        (const uint8_t *)info, strlen(info),
                        out, out_len);
}

void mn_root_expand(mn_channel_t *ch) {
    uint8_t sel[2], base[32];
    hkdf32(ch->root, 32, "selector", sel, 2);
    ch->selector = (uint16_t)((sel[0] << 8) | sel[1]);
    hkdf32(ch->root, 32, "mcast", ch->mcast_suffix, 4);
    hkdf32(ch->root, 32, "chan-base", base, 32);
    /* epoch 0 key; rotation derives epoch‖e from base (§11.1.8, later E-D) */
    uint8_t info[8] = { 'e','p','o','c','h', 0, 0, 0 };
    mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                 NULL, 0, base, sizeof(base), info, sizeof(info),
                 ch->epoch_key, sizeof(ch->epoch_key));
    memset(base, 0, sizeof(base));
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
        /* canonical form: single spaces; caller passes the raw phrase */
        if (hkdf32((const uint8_t *)cred, cred_len, "root", out->root, 32) != 0)
            return -3;
        out->cred_path = 'C';
        size_t n = 0;
        while (n < cred_len && cred[n] != ' ' && n < sizeof(out->name) - 1) n++;
        memcpy(out->name, cred, n);
        out->name[n] = '\0';
    } else {                                                 /* Path B */
        uint8_t salt[32];
        /* salt bound to the channel name (= the passphrase itself when no
         * separate name is given, §11.1.2) */
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
    mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                 NULL, 0, base, sizeof(base), info, sizeof(info),
                 ch->epoch_key, sizeof(ch->epoch_key));
    memset(base, 0, sizeof(base));
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

static int ccm_run(bool enc, const uint8_t key[16], const uint8_t nonce13[13],
                   const uint8_t aad16[16], const uint8_t *in, size_t len,
                   uint8_t *out, uint8_t mic[8]) {
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0) {
        rc = enc ? mbedtls_ccm_encrypt_and_tag(&ctx, len, nonce13, 13, aad16, 16,
                                               in, out, mic, 8)
                 : mbedtls_ccm_auth_decrypt(&ctx, len, nonce13, 13, aad16, 16,
                                            in, out, mic, 8);
    }
    mbedtls_ccm_free(&ctx);
    return rc;
}

int mn_aead_encrypt(const uint8_t key[16], const uint8_t nonce13[13],
                    const uint8_t aad16[16], const uint8_t *pt, size_t len,
                    uint8_t *ct, uint8_t mic[8]) {
    return ccm_run(true, key, nonce13, aad16, pt, len, ct, mic);
}

int mn_aead_decrypt(const uint8_t key[16], const uint8_t nonce13[13],
                    const uint8_t aad16[16], const uint8_t *ct, size_t len,
                    uint8_t *pt, const uint8_t mic[8]) {
    return ccm_run(false, key, nonce13, aad16, ct, len, pt, (uint8_t *)mic);
}

/* ================= device identity: deterministic ECDSA P-256 ============= */
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "esp_random.h"

static mbedtls_mpi       s_d;        /* private scalar */
static mbedtls_ecp_point s_Q;        /* public point   */
static mbedtls_ecp_group s_grp;
static bool s_ident_ready = false;

static int esp_rng(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

int mn_ident_load_or_gen(uint8_t pub65[65], uint8_t device_id[4]) {
    mbedtls_ecp_group_init(&s_grp);
    mbedtls_mpi_init(&s_d);
    mbedtls_ecp_point_init(&s_Q);
    if (mbedtls_ecp_group_load(&s_grp, MBEDTLS_ECP_DP_SECP256R1) != 0) return -1;

    uint8_t dbuf[32];
    size_t blen = sizeof(dbuf);
    nvs_handle_t h;
    bool have = false;
    if (nvs_open("magnet", NVS_READONLY, &h) == ESP_OK) {
        have = (nvs_get_blob(h, "idkey", dbuf, &blen) == ESP_OK && blen == 32);
        nvs_close(h);
    }
    if (have) {
        if (mbedtls_mpi_read_binary(&s_d, dbuf, 32) != 0) return -2;
        if (mbedtls_ecp_mul(&s_grp, &s_Q, &s_d, &s_grp.G, esp_rng, NULL) != 0) return -2;
    } else {
        if (mbedtls_ecp_gen_keypair(&s_grp, &s_d, &s_Q, esp_rng, NULL) != 0) return -3;
        if (mbedtls_mpi_write_binary(&s_d, dbuf, 32) != 0) return -3;
        if (nvs_open("magnet", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "idkey", dbuf, 32);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    memset(dbuf, 0, sizeof(dbuf));

    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&s_grp, &s_Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, pub65, 65) != 0 || olen != 65) return -4;
    uint8_t hash[32];
    mbedtls_sha256(pub65, 65, hash, 0);
    memcpy(device_id, hash, 4);          /* §11.1.1: id = SHA256(pk)[0:4] */
    s_ident_ready = true;
    return 0;
}

int mn_sign(const uint8_t *msg, size_t len, uint8_t sig64[64]) {
    if (!s_ident_ready) return -1;
    uint8_t hash[32];
    mbedtls_sha256(msg, len, hash, 0);
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int rc = mbedtls_ecdsa_sign_det_ext(&s_grp, &r, &s, &s_d, hash, 32,
                                        MBEDTLS_MD_SHA256, esp_rng, NULL);
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&r, sig64, 32) ||
             mbedtls_mpi_write_binary(&s, sig64 + 32, 32);
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return rc ? -2 : 0;
}

int mn_verify(const uint8_t pub65[65], const uint8_t *msg, size_t len,
              const uint8_t sig64[64]) {
    uint8_t hash[32];
    mbedtls_sha256(msg, len, hash, 0);
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) ||
             mbedtls_ecp_point_read_binary(&grp, &Q, pub65, 65) ||
             mbedtls_mpi_read_binary(&r, sig64, 32) ||
             mbedtls_mpi_read_binary(&s, sig64 + 32, 32) ||
             mbedtls_ecdsa_verify(&grp, hash, 32, &Q, &r, &s);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return rc ? -1 : 0;
}
