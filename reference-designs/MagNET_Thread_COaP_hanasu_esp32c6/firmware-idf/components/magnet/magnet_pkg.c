/*
 * magnet_pkg.c — OTA package (.mnpkg) verification. See magnet_pkg.h and
 * docs/OTA-PACKAGE.md (wire format 1, document rev 1.1).
 *
 * Portable: bytes, keys and three crypto calls. Ed25519 comes from the shared
 * TweetNaCl component (magnet_ed25519_verify), P-256 and SHA-256 from the
 * core's platform crypto (mn_verify / mn_sha256_*: mbedTLS on IDF, PSA on
 * Zephyr). tools/pkgtest runs this same file on the host against packages
 * built by tools/mnpkg.py.
 */
#include "magnet_pkg.h"
#include "magnet_pkg_keys.h"
#include "magnet_crypto.h"          /* the CORE one: mn_verify, mn_sha256_* */

#include <string.h>

/* From the TweetNaCl component. Declared here rather than included: that
 * component's header is ALSO named magnet_crypto.h. */
bool magnet_ed25519_verify(const uint8_t pub[32], const uint8_t sig[64],
                           const uint8_t *msg, size_t msg_len);

/* ---- release-key store (§6) ---- */
typedef struct {
    uint8_t        alg;
    uint8_t        id[8];
    uint8_t        len;
    const uint8_t *pub;
} pkg_key_t;

static const uint8_t k0_pub[] = MN_PKG_KEY0_PUB;
#ifdef MN_PKG_KEY1_PUB
static const uint8_t k1_pub[] = MN_PKG_KEY1_PUB;
#endif
static const pkg_key_t s_keys[] = {
    { MN_PKG_KEY0_ALG, MN_PKG_KEY0_ID, MN_PKG_KEY0_LEN, k0_pub },
#ifdef MN_PKG_KEY1_PUB
    { MN_PKG_KEY1_ALG, MN_PKG_KEY1_ID, MN_PKG_KEY1_LEN, k1_pub },
#endif
};

static const pkg_key_t *key_lookup(const uint8_t id[8]) {
    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++)
        if (!memcmp(s_keys[i].id, id, 8)) return &s_keys[i];
    return NULL;
}

/* ---- big-endian field readers ---- */
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void ver(const uint8_t *p, mn_pkg_ver_t *v) {
    v->major = p[0];
    v->minor = p[1];
    v->revision = be16(p + 2);
    v->build = be32(p + 4);
}
static void cstr(char *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n);
    dst[n] = '\0';                     /* dst is n+1; NUL padding ends it early */
}
static bool all_zero(const uint8_t *p, size_t n) {
    while (n--) if (*p++) return false;
    return true;
}

const char *mn_pkg_rc_name(int rc) {
    static const char *const names[] = {
        "OK", "E_PKG_FORMAT", "E_PKG_KEY", "E_PKG_SIG", "E_PKG_TARGET",
        "E_PKG_REPARTITION", "E_PKG_SIZE", "E_PKG_SHA", "E_PKG_IMAGE", "E_PKG_MIN",
    };
    return (rc >= 0 && rc < (int)(sizeof(names) / sizeof(names[0]))) ? names[rc] : "E_PKG_?";
}

int mn_pkg_ver_cmp(const mn_pkg_ver_t *a, const mn_pkg_ver_t *b) {
    if (a->major != b->major)       return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor)       return a->minor < b->minor ? -1 : 1;
    if (a->revision != b->revision) return a->revision < b->revision ? -1 : 1;
    if (a->build != b->build)       return a->build < b->build ? -1 : 1;
    return 0;
}

int mn_pkg_check_header(const uint8_t raw[MN_PKG_HDR_LEN], uint32_t transfer_len,
                        const mn_pkg_self_t *self, mn_pkg_hdr_t *h) {
    memset(h, 0, sizeof(*h));
    h->format_version = raw[4];
    h->sig_alg        = raw[5];
    h->header_len     = be16(raw + 6);
    h->chip           = be16(raw + 8);
    h->board          = be16(raw + 10);
    h->image_format   = raw[12];
    h->flags          = raw[13];
    ver(raw + 16, &h->version);
    ver(raw + 24, &h->min_running);
    h->image_len      = be32(raw + 32);
    memcpy(h->image_sha256, raw + 36, 32);
    h->build_time     = (int64_t)((uint64_t)be32(raw + 68) << 32 | be32(raw + 72));
    cstr(h->variant, raw + 76, 16);
    cstr(h->fw_version, raw + 92, 32);
    memcpy(h->key_id, raw + 124, 8);

    /* 1 — structure. Unknown flags and non-zero reserved bytes are refused so
     * a v1 reader never silently accepts a field it does not understand. */
    const uint8_t known = MN_PKG_FLAG_REQUIRES_REPARTITION | MN_PKG_FLAG_CLEARS_BUNDLES;
    if (memcmp(raw, "MNPK", 4) || h->format_version != 1 ||
        h->header_len != MN_PKG_HDR_LEN ||
        (h->sig_alg != MN_PKG_SIG_P256 && h->sig_alg != MN_PKG_SIG_ED25519) ||
        (h->flags & ~known) || !all_zero(raw + 14, 2) || !all_zero(raw + 132, 60))
        return MN_PKG_E_FORMAT;

    /* 2 — a key we trust */
    const pkg_key_t *k = key_lookup(h->key_id);
    if (!k) return MN_PKG_E_KEY;

    /* 3 — the signature, under the key's OWN algorithm: a key never verifies
     * under the other sig_alg (§4). Ed25519 signs the 192 bytes themselves;
     * P-256 signs their SHA-256 (mn_verify hashes internally). */
    if (h->sig_alg != k->alg) return MN_PKG_E_SIG;
    bool sig_ok = (k->alg == MN_PKG_SIG_ED25519)
        ? magnet_ed25519_verify(k->pub, raw + MN_PKG_SIGNED_LEN, raw, MN_PKG_SIGNED_LEN)
        : mn_verify(k->pub, raw, MN_PKG_SIGNED_LEN, raw + MN_PKG_SIGNED_LEN) == 0;
    if (!sig_ok) return MN_PKG_E_SIG;

    /* 4 — for this chip, this board (or any), this image format */
    if (h->chip != self->chip || (h->board != 0 && h->board != self->board) ||
        h->image_format != self->image_format)
        return MN_PKG_E_TARGET;

    /* 5 — never over the air */
    if (h->flags & MN_PKG_FLAG_REQUIRES_REPARTITION) return MN_PKG_E_REPARTITION;

    /* 6 — the transfer is exactly header + payload, and the payload fits */
    if ((uint64_t)h->image_len + MN_PKG_HDR_LEN != transfer_len ||
        h->image_len > self->slot_size)
        return MN_PKG_E_SIZE;

    return MN_PKG_OK;
}

int mn_pkg_check_payload(const mn_pkg_hdr_t *h, mn_pkg_read_fn rd, void *ctx) {
    mn_sha256_t c;
    uint8_t blk[512], dig[32];
    if (mn_sha256_start(&c) != 0) return MN_PKG_E_SHA;
    for (uint32_t off = 0; off < h->image_len; off += sizeof(blk)) {
        size_t n = h->image_len - off < sizeof(blk) ? h->image_len - off : sizeof(blk);
        if (rd(ctx, off, blk, n) != 0 || mn_sha256_update(&c, blk, n) != 0) {
            (void)mn_sha256_finish(&c, dig);     /* release the context */
            return MN_PKG_E_SHA;
        }
    }
    if (mn_sha256_finish(&c, dig) != 0) return MN_PKG_E_SHA;
    return memcmp(dig, h->image_sha256, 32) ? MN_PKG_E_SHA : MN_PKG_OK;
}

int mn_pkg_check_min_running(const mn_pkg_hdr_t *h, const mn_pkg_self_t *self) {
    return mn_pkg_ver_cmp(&self->running, &h->min_running) < 0 ? MN_PKG_E_MIN : MN_PKG_OK;
}
