#ifndef CRAW_ROLE_BUNDLE_KEYS_H
#define CRAW_ROLE_BUNDLE_KEYS_H

// Trust store. Each entry pairs an author tag with the secret/key used to
// verify their signatures. v1 uses HMAC-SHA256 (shared secret); v2 will add
// Ed25519 entries with per-author 32-byte public keys. The schema below is
// already alg-aware so v2 doesn't break existing entries.
//
// Production deployments should replace the dev key with a per-deployment
// secret OR (better) move to v2 Ed25519 with offline-held private keys.

#include <stdint.h>
#include <stddef.h>

typedef enum {
    TRUST_ALG_HMAC_SHA256 = 1,
    TRUST_ALG_ED25519     = 2,   // not yet implemented (v2)
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

static const craw_role_bundle_trust_entry_t CRAW_ROLE_BUNDLE_TRUST_STORE[] = {
    { "iotone-dev", TRUST_ALG_HMAC_SHA256, CRAW_ROLE_BUNDLE_DEV_HMAC_KEY, 32 },
};

#define CRAW_ROLE_BUNDLE_TRUST_COUNT \
    (sizeof(CRAW_ROLE_BUNDLE_TRUST_STORE) / sizeof(CRAW_ROLE_BUNDLE_TRUST_STORE[0]))

#endif
