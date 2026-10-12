#ifndef EAR_AUDIO_H
#define EAR_AUDIO_H

/* ear_audio — capture pipeline: PDM mic (craw_mic) → DC blocker → gain →
 * IMA-ADPCM → sink, one frame at a time on a dedicated task. The mic runs
 * only while a stream is open (privacy + power). */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ima_adpcm.h"

#define EAR_AUDIO_RATE        16000
#define EAR_AUDIO_MAX_FRAME   244          /* ADPCM bytes: MTU 247 - 3 */

/* Called on the audio task for every encoded frame. `start` is the codec
 * state BEFORE this frame (what an AUDIO_SYNC for this frame must carry). */
typedef void (*ear_audio_sink_t)(const uint8_t *adpcm, size_t nbytes,
                                 const ima_state_t *start, uint16_t frame_no,
                                 void *ctx);

typedef struct {
    uint32_t frames;
    int      dc_raw;      /* mean of raw PCM (mic DC offset), last frame */
    int      rms;         /* post DC-block, pre-gain, last frame */
    int      peak;        /* post-gain, max since start */
    uint32_t clips;       /* post-gain saturations since start */
} ear_audio_stats_t;

int  ear_audio_init(void);
/* frame_bytes ADPCM bytes per frame (even, <= EAR_AUDIO_MAX_FRAME) =
 * 2*frame_bytes samples. Encoder state resets to {0,0} at each start. */
int  ear_audio_start(size_t frame_bytes, ear_audio_sink_t sink, void *ctx);
void ear_audio_stop(void);           /* returns once the sink won't be called again */
bool ear_audio_running(void);

void ear_audio_set_gain(int g);      /* AGC ceiling 1..64, linear */
int  ear_audio_gain(void);
int  ear_audio_gain_now(void);       /* AGC's current gain */
void ear_audio_get_stats(ear_audio_stats_t *out);

#endif
