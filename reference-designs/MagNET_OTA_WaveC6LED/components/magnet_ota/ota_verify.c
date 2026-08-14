/*
 * ota_verify — fetch a pending release and prove it is genuine before anything
 * is allowed to run it.
 *
 * THE PLAN SAID "Ed25519 via mbedTLS". THAT IS NOT POSSIBLE: mbedTLS ships
 * Curve25519 (X25519 key exchange) and has NO EdDSA signature implementation at
 * all. Checked, not assumed, after being wrong once already about this board's
 * display driver. TweetNaCl is vendored beside this file instead — public
 * domain, single file, verify-only path is ~50 lines of use.
 *
 * What the server actually signs (src/releases.lisp, sign-release): the
 * 64-character LOWERCASE HEX STRING of the plaintext SHA256, as UTF-8 bytes.
 * Not the raw digest. Signing the hex text rather than the digest bytes is
 * unusual enough that getting it wrong produces a valid-looking failure, so it
 * is spelled out here.
 *
 * Order is signature-first, then body — the two-step the check-in endpoint was
 * designed around, so a constrained device can reject a bad descriptor before
 * spending bandwidth on the payload.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "magnet_ota.h"
#include "magnet_transport.h"
#include "magnet_cfg.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "magnet_crypto.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "ota_verify";

/* Forth bundles are SOURCE — the aurora demo is 231 bytes. A cap keeps a
 * mis-tagged 2 MB binary from exhausting the heap, and says so out loud rather
 * than dying in malloc. */
#define BUNDLE_MAX 65536

static uint8_t *s_bundle;
static size_t   s_bundle_len;
static char     s_verify_status[80] = "not verified";

const char *ota_verify_status(void) { return s_verify_status; }
const uint8_t *ota_bundle(size_t *len) {
    if (len) *len = s_bundle_len;
    return s_bundle;
}

static int hex2bin(const char *hex, uint8_t *out, size_t out_len) {
    if (strlen(hex) != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; ++i) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void bin2hex(const uint8_t *in, size_t n, char *out) {
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[i * 2]     = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

bool ota_fetch_and_verify(magnet_transport_t *tx, const ota_release_t *rel) {
    if (!tx || !rel) return false;

    char sha_expected[72] = "", sig_hex[136] = "";
    char pub_hex[CFG_MAX];

    if (!cfg_get(CFG_ORG_PUBKEY, pub_hex, sizeof pub_hex)) {
        snprintf(s_verify_status, sizeof s_verify_status, "no org_pubkey");
        return false;
    }

    /* --- 1. Signature first (small), so a bad descriptor costs no payload --- */
    {
        static char resp[1024];
        size_t rlen = 0;
        int st = tx->request(tx, "GET", rel->signature_url, NULL,
                             resp, sizeof resp, &rlen);
        if (st != 200) {
            snprintf(s_verify_status, sizeof s_verify_status, "sig http %d", st);
            return false;
        }
        cJSON *j = cJSON_ParseWithLength(resp, rlen);
        if (!j) { snprintf(s_verify_status, sizeof s_verify_status, "sig bad json"); return false; }
        cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "sha256");
        cJSON *b = cJSON_GetObjectItemCaseSensitive(j, "signature");
        if (cJSON_IsString(a)) { strncpy(sha_expected, a->valuestring, sizeof sha_expected - 1); }
        if (cJSON_IsString(b)) { strncpy(sig_hex, b->valuestring, sizeof sig_hex - 1); }
        cJSON_Delete(j);
        if (strlen(sha_expected) != 64 || strlen(sig_hex) != 128) {
            snprintf(s_verify_status, sizeof s_verify_status, "sig malformed");
            return false;
        }
    }

    /* --- 2. Body --- */
    free(s_bundle); s_bundle = NULL; s_bundle_len = 0;
    size_t cap = BUNDLE_MAX;
    s_bundle = malloc(cap + 1);
    if (!s_bundle) { snprintf(s_verify_status, sizeof s_verify_status, "no memory"); return false; }

    size_t got = 0;
    int st = tx->request(tx, "GET", rel->download_url, NULL,
                         (char *)s_bundle, cap + 1, &got);
    if (st != 200) {
        snprintf(s_verify_status, sizeof s_verify_status, "body http %d", st);
        free(s_bundle); s_bundle = NULL;
        return false;
    }
    s_bundle_len = got;

    /* --- 3. Does the body match the descriptor's digest? --- */
    uint8_t digest[32];
    char digest_hex[72];
    mbedtls_sha256(s_bundle, s_bundle_len, digest, 0);
    bin2hex(digest, sizeof digest, digest_hex);
    if (strcmp(digest_hex, sha_expected) != 0) {
        /* Content does not match what was signed. Could be corruption or
         * substitution; from here they are indistinguishable and both are fatal. */
        snprintf(s_verify_status, sizeof s_verify_status, "SHA256 MISMATCH");
        ESP_LOGE(TAG, "sha256 %s != expected %s", digest_hex, sha_expected);
        free(s_bundle); s_bundle = NULL; s_bundle_len = 0;
        return false;
    }

    /* --- 4. Is that digest signed by our org's key? --- */
    uint8_t pub[32], sig[64];
    if (hex2bin(pub_hex, pub, sizeof pub) != 0 ||
        hex2bin(sig_hex, sig, sizeof sig) != 0) {
        snprintf(s_verify_status, sizeof s_verify_status, "key/sig not hex");
        free(s_bundle); s_bundle = NULL; s_bundle_len = 0;
        return false;
    }

    /* The signed message is the 64-char hex TEXT of the digest, not the 32
     * raw digest bytes — see the header comment. Verification now goes
     * through the shared magnet_crypto component (punch-list H3). */
    if (!magnet_ed25519_verify(pub, sig, (const uint8_t *)digest_hex, 64)) {
        snprintf(s_verify_status, sizeof s_verify_status, "SIGNATURE BAD");
        ESP_LOGE(TAG, "Ed25519 verify FAILED for release %ld", rel->release_id);
        free(s_bundle); s_bundle = NULL; s_bundle_len = 0;
        return false;
    }

    snprintf(s_verify_status, sizeof s_verify_status, "SIGNATURE OK (%u B)",
             (unsigned)s_bundle_len);
    ESP_LOGI(TAG, "release %ld verified: %u bytes, sha256 ok, Ed25519 ok",
             rel->release_id, (unsigned)s_bundle_len);
    return true;
}
