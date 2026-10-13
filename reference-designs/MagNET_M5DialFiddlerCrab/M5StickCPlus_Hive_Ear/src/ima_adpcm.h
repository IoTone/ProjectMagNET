#ifndef IMA_ADPCM_H
#define IMA_ADPCM_H

/* IMA/DVI ADPCM, 4 bits/sample, high nibble first — the ATVV wire codec.
 * State is continuous across frames (ATVV v1.0 frames are headerless;
 * the receiver re-anchors only on AUDIO_SYNC {predictor, step_index}). */

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int16_t predictor;
    uint8_t step_index;   /* 0..88 */
} ima_state_t;

/* Encode n samples (n even) into n/2 bytes. Updates *st. */
void ima_encode(ima_state_t *st, const int16_t *pcm, size_t n, uint8_t *out);

/* Decode nbytes into 2*nbytes samples. Updates *st. */
void ima_decode(ima_state_t *st, const uint8_t *in, size_t nbytes, int16_t *pcm);

#endif
