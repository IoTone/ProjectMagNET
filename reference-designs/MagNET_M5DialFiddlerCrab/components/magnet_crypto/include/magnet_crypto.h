#ifndef MAGNET_CRYPTO_H
#define MAGNET_CRYPTO_H

// magnet_crypto — shared signature primitives for MagNET code delivery.
//
// Factored out of MagNET_OTA_WaveC6LED's magnet_ota (punch-list H3) so that
// both code-upgrade paths — the hive's craw_role_bundle and the OTA
// check-in loop — verify through one implementation. Ed25519 is provided by
// vendored TweetNaCl (mbedTLS ships no EdDSA; see the incident note in
// ota_verify.c). Verification uses no randomness, but randombytes() is wired
// to the hardware RNG anyway so any future keygen use is safe, never silent.
//
// Threat model reminder: an Ed25519 signature proves the AUTHOR (private key
// held offline) — unlike the v1 hive HMAC, where anyone able to verify can
// also forge.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Verify a detached Ed25519 signature over msg.
//   pub: 32-byte public key
//   sig: 64-byte signature
// Returns true iff the signature is valid. Allocates 2*(64+msg_len) bytes
// transiently (TweetNaCl verifies a combined sig||msg buffer); returns false
// on allocation failure.
bool magnet_ed25519_verify(const uint8_t pub[32], const uint8_t sig[64],
                           const uint8_t *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif
#endif
