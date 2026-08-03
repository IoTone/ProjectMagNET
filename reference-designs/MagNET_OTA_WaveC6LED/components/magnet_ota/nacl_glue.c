/*
 * TweetNaCl needs a randombytes() from its host. We only ever VERIFY
 * signatures, which uses no randomness — but the symbol still has to resolve,
 * and a stub that quietly returns zeros would be a catastrophe if anything ever
 * did call it to generate a key.
 *
 * So: wire it to the hardware RNG, which is correct, and cheap.
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_random.h"

void randombytes(unsigned char *x, unsigned long long n) {
    esp_fill_random(x, (size_t)n);
}
