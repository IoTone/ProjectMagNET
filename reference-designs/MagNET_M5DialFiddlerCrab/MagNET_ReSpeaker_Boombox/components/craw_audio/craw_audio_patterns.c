/*
 * craw_audio_patterns — built-in notification recipes.
 *
 * Each recipe is a static-const segment array. Adding a new one is a
 * three-line change: define the array + a public play_X function + an
 * extern declaration in craw_audio.h.
 *
 * Authoring tips:
 *   - Notification frequencies in 600–2200 Hz are most audible on small
 *     speakers; under 400 Hz tends to sound muffled.
 *   - Keep each recipe under 1.5 s so they don't pile up if triggered
 *     repeatedly. Boot chirp (sunrise) is the deliberate exception.
 *   - Use envelopes (gain → gain_end) on the FIRST and LAST segment of
 *     a recipe to avoid click artifacts at the start/stop.
 */

#include "craw_audio.h"

/* Three rising chirps with short gaps. Says "look at me." */
static const craw_audio_seg_t ALERT_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 =  600, .f1 = 1200, .ms = 200, .gain = 0.0f, .gain_end = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SLEEP,                                .ms =  80 },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 =  600, .f1 = 1200, .ms = 200, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SLEEP,                                .ms =  80 },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 =  600, .f1 = 1200, .ms = 200, .gain = 0.7f, .gain_end = 0.0f },
};

/* Short two-beep "ding-ding". Friendly nudge. */
static const craw_audio_seg_t NOTIFY_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 1500, .ms =  80, .gain = 0.0f, .gain_end = 0.6f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 1500, .ms =  20, .gain = 0.6f, .gain_end = 0.0f },
    { .kind = CRAW_AUDIO_SEG_SLEEP,                       .ms = 100 },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 1500, .ms =  80, .gain = 0.0f, .gain_end = 0.6f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 1500, .ms =  20, .gain = 0.6f, .gain_end = 0.0f },
};

/* AM siren: 1 kHz carrier modulated at 4 Hz, 800 ms. Sounds like an alarm. */
static const craw_audio_seg_t WARN_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_AM, .f0 = 1000, .f1 = 4, .ms = 800, .gain = 0.0f, .gain_end = 0.6f },
    { .kind = CRAW_AUDIO_SEG_AM, .f0 = 1000, .f1 = 4, .ms =  40, .gain = 0.6f, .gain_end = 0.0f },
};

/* Descending sweep + held low note. "Something broke." */
static const craw_audio_seg_t ERROR_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = 800, .f1 = 350, .ms = 400, .gain = 0.0f, .gain_end = 0.7f },
    { .kind = CRAW_AUDIO_SEG_TONE,  .f0 = 350,            .ms = 200, .gain = 0.7f, .gain_end = 0.0f },
};

/* Wail-style emergency siren: two full up/down cycles between 500 Hz
 * and 1500 Hz, each sweep 600 ms. Envelope-in on the first sweep and
 * envelope-out on the last so it starts and ends without a click. ~2.4 s
 * total. Distinct from `warn` (AM-modulated alarm beep at fixed pitch). */
static const craw_audio_seg_t SIREN_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 =  500, .f1 = 1500, .ms = 600, .gain = 0.0f, .gain_end = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = 1500, .f1 =  500, .ms = 600, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 =  500, .f1 = 1500, .ms = 600, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = 1500, .f1 =  500, .ms = 600, .gain = 0.7f, .gain_end = 0.0f },
};

/* Yelp: fast variant of siren. Three full cycles, 200 ms per sweep, ~1.2 s.
 * The urgent "wee-woo-wee-woo" used by police vehicles approaching at speed. */
static const craw_audio_seg_t YELP_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 =  500, .f1 = 1500, .ms = 200, .gain = 0.0f, .gain_end = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = 1500, .f1 =  500, .ms = 200, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 =  500, .f1 = 1500, .ms = 200, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = 1500, .f1 =  500, .ms = 200, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 =  500, .f1 = 1500, .ms = 200, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = 1500, .f1 =  500, .ms = 200, .gain = 0.7f, .gain_end = 0.0f },
};

/* European two-tone "nee-naw": alternating discrete tones at 950 Hz and
 * 750 Hz (a perfect fourth apart), 400 ms each, three full cycles.
 * Envelope on first/last for click-free start/stop; the inner-tone
 * boundary clicks are masked by the alarm aesthetic. ~2.4 s. */
static const craw_audio_seg_t NEE_NAW_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 950, .ms = 400, .gain = 0.0f, .gain_end = 0.7f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 750, .ms = 400, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 950, .ms = 400, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 750, .ms = 400, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 950, .ms = 400, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 750, .ms = 400, .gain = 0.7f, .gain_end = 0.0f },
};

/* Air-raid: slow ominous rise → hold at peak → slow fall, single cycle.
 * 1.5 s rise + 0.8 s hold + 1.5 s fall = ~3.8 s. Crescendo-then-fade
 * envelope reinforces the impression of an approaching/receding source. */
static const craw_audio_seg_t AIR_RAID_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = 300, .f1 = 800, .ms = 1500, .gain = 0.0f, .gain_end = 0.7f },
    { .kind = CRAW_AUDIO_SEG_TONE,  .f0 = 800,            .ms =  800, .gain = 0.7f },
    { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = 800, .f1 = 300, .ms = 1500, .gain = 0.7f, .gain_end = 0.0f },
};

/* Sunrise: ascending C-major arpeggio (C4-E4-G4-C5) with a crescendo
 * envelope that eases in, peaks at G4, and fades out on C5. ~1.4 s.
 * Pleasant, recognizable, doesn't startle.
 *   C4 ≈ 261.63 Hz
 *   E4 ≈ 329.63 Hz
 *   G4 ≈ 392.00 Hz
 *   C5 ≈ 523.25 Hz
 */
static const craw_audio_seg_t SUNRISE_SEGS[] = {
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 262, .ms = 240, .gain = 0.00f, .gain_end = 0.30f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 330, .ms = 220, .gain = 0.30f, .gain_end = 0.45f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 392, .ms = 220, .gain = 0.45f, .gain_end = 0.55f },
    { .kind = CRAW_AUDIO_SEG_TONE, .f0 = 523, .ms = 700, .gain = 0.55f, .gain_end = 0.00f },
};

#define ARR_LEN(a) ((int)(sizeof(a)/sizeof((a)[0])))

int craw_audio_play_alert(void)    { return craw_audio_play_pattern(ALERT_SEGS,    ARR_LEN(ALERT_SEGS));    }
int craw_audio_play_notify(void)   { return craw_audio_play_pattern(NOTIFY_SEGS,   ARR_LEN(NOTIFY_SEGS));   }
int craw_audio_play_warn(void)     { return craw_audio_play_pattern(WARN_SEGS,     ARR_LEN(WARN_SEGS));     }
int craw_audio_play_error(void)    { return craw_audio_play_pattern(ERROR_SEGS,    ARR_LEN(ERROR_SEGS));    }
int craw_audio_play_siren(void)    { return craw_audio_play_pattern(SIREN_SEGS,    ARR_LEN(SIREN_SEGS));    }
int craw_audio_play_yelp(void)     { return craw_audio_play_pattern(YELP_SEGS,     ARR_LEN(YELP_SEGS));     }
int craw_audio_play_nee_naw(void)  { return craw_audio_play_pattern(NEE_NAW_SEGS,  ARR_LEN(NEE_NAW_SEGS));  }
int craw_audio_play_air_raid(void) { return craw_audio_play_pattern(AIR_RAID_SEGS, ARR_LEN(AIR_RAID_SEGS)); }
int craw_audio_play_sunrise(void)  { return craw_audio_play_pattern(SUNRISE_SEGS,  ARR_LEN(SUNRISE_SEGS));  }
