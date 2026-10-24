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
 *
 * OVER THE RELAY THE BODY COMES IN RANGED CHUNKS (#101). One request for the
 * whole body fails outright on a single lost frame — at 1 % frame loss a 64 KB
 * bundle (131 frames at MTU 517) gets through 27 % of the time. Instead the
 * body is asked for as ?offset=&length= slices (RobotARme server >= 0.139.0
 * answers 206) and the slice size adapts, AIMD as TCP's window does: start at
 * CHUNK_START, halve on a failed chunk down to CHUNK_MIN, grow by CHUNK_STEP
 * after each clean one up to CHUNK_MAX. A lost frame then costs one chunk.
 * Measured in the RobotARme Mobile OTA fork (528 KB image, injected loss):
 * every run completed at up to 5 % loss, where one request never did; doubling
 * instead of +CHUNK_STEP overshot into chunks that fail and was slower.
 * Direct IP keeps the single request: TCP already retransmits.
 *
 * A failure is either the PATH's (link dropped, chunk retries exhausted, URL
 * expired) or the RELEASE's (bad descriptor, digest or signature).
 * ota_verify_failure_transient() says which, so the poll loop does not
 * blacklist a good release because a phone walked away.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "magnet_ota.h"
#include "magnet_transport.h"
#include "magnet_relay.h"
#include "magnet_cfg.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "magnet_crypto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_verify";

/* Forth bundles are SOURCE — the aurora demo is 231 bytes. A cap keeps a
 * mis-tagged 2 MB binary from exhausting the heap, and says so out loud rather
 * than dying in malloc. */
#define BUNDLE_MAX 65536

/* Ranged chunks over the relay (see the header comment). */
#define CHUNK_START   16384
#define CHUNK_MIN     1024
#define CHUNK_MAX     65536
#define CHUNK_STEP    2048
#define CHUNK_TRIES   8
#define RELAY_WAIT_MS 120000     /* a proxy that dropped mid-fetch may come back */

static uint8_t *s_bundle;
static size_t   s_bundle_len;
static char     s_verify_status[80] = "not verified";
static bool     s_transient;

const char *ota_verify_status(void) { return s_verify_status; }
bool ota_verify_failure_transient(void) { return s_transient; }
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

/* One chunk's destination: the slice [base, base + want) of s_bundle. */
typedef struct {
    size_t base, want, got;
    bool   overrun;          /* more than WANT arrived: the server ignored the range */
} chunk_t;

/* Runs in the pipe's RX path, once per frame (relay_request_stream). */
static bool chunk_sink(void *ctx, const uint8_t *data, size_t n) {
    chunk_t *c = ctx;
    if (c->got + n > c->want) { c->overrun = true; return false; }
    memcpy(s_bundle + c->base + c->got, data, n);
    c->got += n;
    return true;
}

static bool wait_for_relay(void) {
    if (relay_available()) return true;
    ESP_LOGW(TAG, "relay gone mid-fetch; waiting up to %d s to resume", RELAY_WAIT_MS / 1000);
    for (int t = 0; t < RELAY_WAIT_MS; t += 250) {
        if (relay_available()) { vTaskDelay(pdMS_TO_TICKS(1500)); return true; }   /* let MTU settle */
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

/* Fetch TOTAL bytes of URL into s_bundle over the relay, in adaptive ranged
 * chunks. 0 on success; otherwise s_verify_status says why. */
static int fetch_ranged(const char *url, size_t total) {
    size_t chunk = CHUNK_START, lo = chunk, hi = chunk;
    unsigned requests = 0, retries = 0;
    char path[sizeof ((ota_release_t *)0)->download_url + 48];
    int64_t t0 = esp_timer_get_time();
    for (size_t off = 0; off < total; ) {
        int tries = 0, code = 0;
        size_t want;
        chunk_t c;
        for (;;) {
            want = total - off < chunk ? total - off : chunk;
            if (want == total) snprintf(path, sizeof path, "%s", url);
            else snprintf(path, sizeof path, "%s%soffset=%u&length=%u", url,
                          strchr(url, '?') ? "&" : "?", (unsigned)off, (unsigned)want);
            if (!wait_for_relay()) {
                snprintf(s_verify_status, sizeof s_verify_status, "relay gone at %u", (unsigned)off);
                return -1;
            }
            c = (chunk_t){ .base = off, .want = want };
            size_t seen = 0;
            code = relay_request_stream("GET", path, NULL, chunk_sink, &c, want, &seen);
            requests++;
            if ((code == 200 || code == 206) && c.got == want) break;
            if (c.overrun && off == 0 && want < total) {
                /* A server older than ranged downloads answers 200 with the
                 * whole body. Fall back to one request, the method before #101. */
                ESP_LOGW(TAG, "server ignores ranges; fetching the whole body");
                chunk = total;
                continue;
            }
            if (code == 403 || code == 404 || code == 416) {
                /* the signed URL expired or the release went away: a fresh
                 * check-in hands out a new one */
                snprintf(s_verify_status, sizeof s_verify_status, "body http %d at %u", code, (unsigned)off);
                return -1;
            }
            retries++;
            chunk = chunk / 2 < CHUNK_MIN ? CHUNK_MIN : chunk / 2;
            if (chunk < lo) lo = chunk;
            if (++tries >= CHUNK_TRIES) {
                snprintf(s_verify_status, sizeof s_verify_status, "chunk at %u failed %d times (last %d)",
                         (unsigned)off, tries, code);
                return -1;
            }
            ESP_LOGW(TAG, "chunk at %u: %d (%u of %u B); retry %d, smaller", (unsigned)off, code,
                     (unsigned)c.got, (unsigned)want, tries);
        }
        off += want;
        if (chunk < CHUNK_MAX) {
            chunk = chunk + CHUNK_STEP > CHUNK_MAX ? CHUNK_MAX : chunk + CHUNK_STEP;
            if (chunk > hi) hi = chunk;
        }
    }
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "fetched %u B in %lld ms (%lld B/s): %u requests, chunk %u..%u B, %u retries",
             (unsigned)total, ms, ms ? (long long)total * 1000 / ms : 0, requests,
             (unsigned)lo, (unsigned)hi, retries);
    return 0;
}

bool ota_fetch_and_verify(magnet_transport_t *tx, const ota_release_t *rel) {
    if (!tx || !rel) return false;
    /* Every return below is the release's fault unless marked transient. */
    s_transient = false;

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
            s_transient = true;
            return false;
        }
        cJSON *j = cJSON_ParseWithLength(resp, rlen);
        if (!j) { snprintf(s_verify_status, sizeof s_verify_status, "sig bad json"); s_transient = true; return false; }
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
    /* SIZE THE BUFFER TO THE RELEASE, not to the cap. A fixed 64 KB malloc
     * for a 67-byte bundle failed on the touch unit (IDF 5.4 + NimBLE +
     * WiFi resident: ~78 KB free, no 64 KB contiguous block) — three times,
     * until the poison counter gave up. The check-in tells us the size;
     * only an unknown size falls back to the cap, and either way the
     * transport flags truncation rather than parsing a clipped body. */
    if (rel->size_bytes > BUNDLE_MAX) {
        snprintf(s_verify_status, sizeof s_verify_status, "bundle %ld B over the %d B cap",
                 rel->size_bytes, BUNDLE_MAX);
        return false;
    }
    size_t cap = BUNDLE_MAX;
    if (rel->size_bytes > 0) cap = (size_t)rel->size_bytes;
    s_bundle = malloc(cap + 1);
    if (!s_bundle) {
        snprintf(s_verify_status, sizeof s_verify_status, "no memory (need %u, largest %u)",
                 (unsigned)(cap + 1),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        ESP_LOGE(TAG, "%s", s_verify_status);
        s_transient = true;
        return false;
    }

    if (tx == transport_relay() && rel->size_bytes > 0) {
        /* Ranged chunks: a lost frame costs one chunk (header comment). */
        if (fetch_ranged(rel->download_url, cap) != 0) {
            free(s_bundle); s_bundle = NULL;
            s_transient = true;
            return false;
        }
        s_bundle_len = cap;
    } else {
        size_t got = 0;
        int st = tx->request(tx, "GET", rel->download_url, NULL,
                             (char *)s_bundle, cap + 1, &got);
        if (st != 200) {
            snprintf(s_verify_status, sizeof s_verify_status, "body http %d", st);
            free(s_bundle); s_bundle = NULL;
            s_transient = true;
            return false;
        }
        s_bundle_len = got;
    }
    s_bundle[s_bundle_len] = '\0';      /* the bundle is Forth source */

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
