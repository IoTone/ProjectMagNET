#ifndef CRAW_ROLE_BUNDLE_KEYS_H
#define CRAW_ROLE_BUNDLE_KEYS_H

// Trust store. Each entry pairs an author tag with the secret/key used to
// verify their signatures. v1 uses HMAC-SHA256 (shared secret); v2 adds
// Ed25519 entries with per-author 32-byte public keys (punch-list H3,
// 2026-08-14). An author may appear once PER ALGORITHM — lookup matches
// (author, alg), which is what lets a node accept both v1-HMAC and
// v2-Ed25519 bundles during the migration window.
//
// Production deployments should carry ONLY Ed25519 entries: the HMAC secret
// proves hive membership, not authorship — anyone who can verify can forge.
// Retire the HMAC row once every publisher signs with a private key.

#include <stdint.h>
#include <stddef.h>

typedef enum {
    TRUST_ALG_HMAC_SHA256 = 1,
    TRUST_ALG_ED25519     = 2,
} craw_role_bundle_alg_t;

typedef struct {
    const char  *author;             // matches "author" field in envelope
    int          alg;                // craw_role_bundle_alg_t
    const uint8_t *key;               // 32 bytes for both HMAC and Ed25519
    size_t       key_len;            // always 32 for current algs
} craw_role_bundle_trust_entry_t;

// Dev key: same bytes as CRAW_HIVE_DEV_SECRET. v1 deliberately reuses it so
// any device that can speak hive can also publish bundles — the model is
// "trusts holders of the hive shared secret." v2 replaces this with proper
// per-author keys.
//
// Defined here as an array of bytes so the linker can place it in flash.
static const uint8_t CRAW_ROLE_BUNDLE_DEV_HMAC_KEY[32] = {
    0xA0, 0x8F, 0x19, 0xC3, 0x4B, 0x55, 0xD7, 0xE1,
    0xF2, 0x0A, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
    0xDD, 0xEE, 0xFF, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD,
};

// Dev Ed25519 PUBLIC key. The matching private seed lives in
// scripts/dev_ed25519.key — a DEV key, committed on purpose (same posture as
// the dev HMAC secret above). Production authors generate their own pair,
// hold the seed offline, and land only the public half here (reflash).
static const uint8_t CRAW_ROLE_BUNDLE_DEV_ED25519_PUB[32] = {
    0x39, 0xc9, 0x2f, 0x1b, 0x50, 0xa1, 0x7f, 0xc7,
    0x07, 0xba, 0xe0, 0xb1, 0x27, 0xbf, 0x07, 0x48,
    0xd0, 0xbb, 0x28, 0x90, 0xef, 0x9c, 0x64, 0xdf,
    0xed, 0x49, 0x49, 0xee, 0x7f, 0xb8, 0x91, 0xa1,
};

static const craw_role_bundle_trust_entry_t CRAW_ROLE_BUNDLE_TRUST_STORE[] = {
    { "iotone-dev", TRUST_ALG_HMAC_SHA256, CRAW_ROLE_BUNDLE_DEV_HMAC_KEY,    32 },
    { "iotone-dev", TRUST_ALG_ED25519,     CRAW_ROLE_BUNDLE_DEV_ED25519_PUB, 32 },
};

#define CRAW_ROLE_BUNDLE_TRUST_COUNT \
    (sizeof(CRAW_ROLE_BUNDLE_TRUST_STORE) / sizeof(CRAW_ROLE_BUNDLE_TRUST_STORE[0]))

#endif
