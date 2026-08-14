/*
 * magnet_crypto — Ed25519 verify over vendored TweetNaCl.
 *
 * Apache License 2.0, Copyright 2026 IoTone, Inc.
 * TweetNaCl itself is public domain (Bernstein, van Gastel, Janssen, Lange,
 * Schwabe, Sprenkels — https://tweetnacl.cr.yp.to/).
 */

#include "magnet_crypto.h"
#include "tweetnacl.h"

#include <stdlib.h>
#include <string.h>

bool magnet_ed25519_verify(const uint8_t pub[32], const uint8_t sig[64],
                           const uint8_t *msg, size_t msg_len) {
    if (!pub || !sig || (!msg && msg_len > 0)) return false;

    /* TweetNaCl's crypto_sign_open wants sig||msg in, and a same-sized
     * scratch buffer out. Bundle signing inputs run to ~6 KB, so these live
     * on the heap rather than the stack of whichever task called us. */
    size_t smlen = 64 + msg_len;
    unsigned char *sm = malloc(smlen);
    unsigned char *m  = malloc(smlen);
    if (!sm || !m) { free(sm); free(m); return false; }

    memcpy(sm, sig, 64);
    if (msg_len) memcpy(sm + 64, msg, msg_len);

    unsigned long long mlen = 0;
    int rc = crypto_sign_open(m, &mlen, sm, (unsigned long long)smlen, pub);

    free(sm);
    free(m);
    return rc == 0;
}
