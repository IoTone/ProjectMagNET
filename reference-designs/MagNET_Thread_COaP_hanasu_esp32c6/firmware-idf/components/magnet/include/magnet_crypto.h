/*
 * magnet_crypto.h — channel credential derivation + AEAD (§11.1, E-Phase D).
 *
 * Credential auto-detection (§11.1.2):
 *   "qr:<base64-32B>"        → Path A: full-entropy secret, no stretch
 *   ≥12 space-separated words → Path C: seed phrase, HKDF only (rev 2.2)
 *   anything else             → Path B: passphrase, PBKDF2 (iters below)
 *
 * All key material stays inside the C core (§12.0) — no Forth access.
 */
#ifndef MAGNET_CRYPTO_H
#define MAGNET_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_PBKDF2_ITERS 100000   /* Path B stretch; tune toward ~1 s on the C6 */

typedef struct {
    bool     set;
    char     cred_path;          /* 'A' | 'B' | 'C' — how the secret arrived   */
    char     name[17];           /* public label (defaults to first word/token) */
    uint8_t  root[32];           /* root_secret — cached in NVS after derive    */
    uint16_t selector;           /* §11.1.3: from root, 16-bit, public on wire  */
    uint8_t  mcast_suffix[4];    /* ff05::<suffix> group                        */
    uint8_t  epoch_key[16];      /* epoch 0 AES-CCM key (rotation = later E-D)  */
} mn_channel_t;

/* Derive everything from a credential. Runs PBKDF2 for Path B (~seconds). */
int mn_cred_derive(const char *cred, size_t cred_len, mn_channel_t *out);

/* Re-derive selector/mcast/epoch_key from an already-known root_secret
 * (NVS-cached channel — skips the stretch entirely). */
void mn_root_expand(mn_channel_t *ch);
void mn_chan_epoch_key(mn_channel_t *ch, uint8_t e);  /* §11.1.8 rotation */

/* §11.1.5 nonce: device_id(4) ‖ counter(4) ‖ epoch(1) ‖ selector(2) ‖ 0x0000 */
void mn_nonce_build(uint8_t nonce13[13], const uint8_t device_id[4],
                    uint32_t counter, uint8_t epoch, uint16_t selector);

/* AES-128-CCM, 8-byte MIC, AAD = the 16-byte envelope header.
 * Encrypt: pt[len] → ct[len] + mic[8].  Decrypt returns 0 iff MIC verifies. */
int mn_aead_encrypt(const uint8_t key[16], const uint8_t nonce13[13],
                    const uint8_t aad16[16], const uint8_t *pt, size_t len,
                    uint8_t *ct, uint8_t mic[8]);
int mn_aead_decrypt(const uint8_t key[16], const uint8_t nonce13[13],
                    const uint8_t aad16[16], const uint8_t *ct, size_t len,
                    uint8_t *pt, const uint8_t mic[8]);

/* ---- device identity: deterministic ECDSA P-256 (decided rev 2.2) ----
 * Private scalar lives in NVS ("idkey"); device_id = SHA256(pub65)[0:4]. */
int mn_ident_load_or_gen(uint8_t pub65[65], uint8_t device_id[4]);
int mn_sign(const uint8_t *msg, size_t len, uint8_t sig64[64]);
int mn_verify(const uint8_t pub65[65], const uint8_t *msg, size_t len,
              const uint8_t sig64[64]);

#ifdef __cplusplus
}
#endif

#endif /* MAGNET_CRYPTO_H */
