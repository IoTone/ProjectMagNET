// ear_audio — capture pipeline. See ear_audio.h.

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "craw_mic.h"
#include "ear_audio.h"

static const char *TAG = "ear_audio";

/* Voice-band high-pass from craw_mic (120 Hz biquad): kills the PDM DC
 * drift that otherwise buries speech. See craw_mic.h. */
#define HPF_HZ   120.0f

/* High-shelf cut: the SPM1423 in the StickC case reads 6-10 dB hot above
 * 4 kHz vs the source (measured 2026-10-07, TTS played to the mic), which
 * is the harsh/hissy sound. -8 dB from ~4.5 kHz restores a natural tilt. */
#define SHELF_HZ  4500.0f
#define SHELF_DB  -8.0f

static void shelf_init(craw_mic_hpf_t *f, float fc, float db, int fs) {   /* RBJ high-shelf, S=1 */
    float A = powf(10.0f, db / 40.0f), w0 = 2.0f * (float)M_PI * fc / (float)fs;
    float c = cosf(w0), al = sinf(w0) / 2.0f * sqrtf(2.0f), sa = 2.0f * sqrtf(A) * al;
    float a0 = (A + 1) - (A - 1) * c + sa;
    f->b0 =  A * ((A + 1) + (A - 1) * c + sa) / a0;
    f->b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    f->b2 =  A * ((A + 1) + (A - 1) * c - sa) / a0;
    f->a1 =  2 * ((A - 1) - (A + 1) * c) / a0;
    f->a2 =  ((A + 1) - (A - 1) * c - sa) / a0;
    f->z1 = f->z2 = 0.0f;
}

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_stopped;     /* given by the task when it parks */
static volatile bool     s_run;
static size_t            s_frame_bytes;
static ear_audio_sink_t  s_sink;
static void             *s_ctx;
static volatile int      s_gain = 16;     /* AGC ceiling (ear-gain) */
static volatile int      s_gain_now = 16; /* AGC current, x16 fixed point below */
static ear_audio_stats_t s_stats;

static int16_t s_pcm[2 * EAR_AUDIO_MAX_FRAME];
static uint8_t s_out[EAR_AUDIO_MAX_FRAME];

static void audio_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_run) { xSemaphoreGive(s_stopped); continue; }

        if (craw_mic_stream_start() != ESP_OK) {
            ESP_LOGE(TAG, "mic start failed");
            s_run = false;
            xSemaphoreGive(s_stopped);
            continue;
        }
        ima_state_t st = { 0, 0 };
        uint16_t frame_no = 0;
        craw_mic_hpf_t hpf; craw_mic_hpf_init(&hpf, HPF_HZ, EAR_AUDIO_RATE);
        craw_mic_hpf_t shelf; shelf_init(&shelf, SHELF_HZ, SHELF_DB, EAR_AUDIO_RATE);
        float dc = 0.0f; bool dc_primed = false;   /* subtract before the HPF: no start thump */
        const size_t n = 2 * s_frame_bytes;
        /* AGC: gain in 1/16 steps. Fast attack when a frame peaks above
         * ~-4 dBFS, slow release (+~6 %/frame after 0.25 s quiet) toward the
         * ear-gain ceiling. Keeps close-talk from clipping and distant voice
         * audible without a manual knob. */
        int g16 = s_gain * 16, quiet = 0;

        while (s_run) {
            size_t got = 0;
            esp_err_t err = craw_mic_stream_read(s_pcm, n, &got, 200);
            if (err != ESP_OK || got != n) {
                ESP_LOGW(TAG, "mic read err=0x%x got=%u/%u", err, (unsigned)got, (unsigned)n);
                continue;
            }
            int64_t sum = 0, sq = 0;
            int peak = 0;
            uint32_t clips = 0;
            for (size_t i = 0; i < n; i++) sum += s_pcm[i];
            float mean = (float)sum / (float)n;
            if (!dc_primed) { dc = mean; dc_primed = true; }
            else dc += 0.05f * (mean - dc);          /* slow per-frame tracker */
            for (size_t i = 0; i < n; i++) {
                float yf = craw_mic_hpf_run(&shelf, craw_mic_hpf_run(&hpf, (float)s_pcm[i] - dc));
                int32_t y = (int32_t)lrintf(yf);
                sq += (int64_t)y * y;
                int32_t v = (y * g16) >> 4;
                if (v > 32767)  { v = 32767;  clips++; }
                if (v < -32768) { v = -32768; clips++; }
                s_pcm[i] = (int16_t)v;
                int a = v < 0 ? -v : v;
                if (a > peak) peak = a;
            }
            if (peak > 20000) { g16 = g16 * 3 / 4; quiet = 0; }
            else if (peak < 8000 && ++quiet > 16) g16 += g16 / 16 + 1;
            else if (peak >= 8000) quiet = 0;
            if (g16 < 16) g16 = 16;
            if (g16 > s_gain * 16) g16 = s_gain * 16;
            s_gain_now = g16 / 16;
            s_stats.frames++;
            s_stats.dc_raw = (int)(sum / (int64_t)n);
            int ms = (int)(sq / (int64_t)n);
            int r = 0; while ((r + 1) * (r + 1) <= ms) r++;   /* small isqrt */
            s_stats.rms = r;
            if (peak > s_stats.peak) s_stats.peak = peak;
            s_stats.clips += clips;

            ima_state_t start = st;
            ima_encode(&st, s_pcm, n, s_out);
            if (s_run && s_sink) s_sink(s_out, s_frame_bytes, &start, frame_no, s_ctx);
            frame_no++;
        }
        craw_mic_stream_stop();
        xSemaphoreGive(s_stopped);
    }
}

int ear_audio_init(void) {
    craw_mic_config_t cfg = { .sample_rate = EAR_AUDIO_RATE };
    esp_err_t err = craw_mic_init(&cfg);
    if (err != ESP_OK) return err;
    s_stopped = xSemaphoreCreateBinary();
    /* prio 6: above UI/REPL, below the NimBLE host (prio ~21). */
    if (xTaskCreate(audio_task, "ear_audio", 4096, NULL, 6, &s_task) != pdPASS) return -1;
    return 0;
}

int ear_audio_start(size_t frame_bytes, ear_audio_sink_t sink, void *ctx) {
    if (s_run) return -1;
    if (frame_bytes < 2 || frame_bytes > EAR_AUDIO_MAX_FRAME || (frame_bytes & 1)) return -2;
    s_frame_bytes = frame_bytes;
    s_sink = sink;
    s_ctx = ctx;
    memset(&s_stats, 0, sizeof(s_stats));
    xSemaphoreTake(s_stopped, 0);   /* clear any stale give */
    s_run = true;
    xTaskNotifyGive(s_task);
    return 0;
}

void ear_audio_stop(void) {
    if (!s_run) return;
    s_run = false;
    xSemaphoreTake(s_stopped, pdMS_TO_TICKS(500));
}

bool ear_audio_running(void) { return s_run; }

void ear_audio_set_gain(int g) { s_gain = g < 1 ? 1 : g > 64 ? 64 : g; }
int  ear_audio_gain_now(void)  { return s_gain_now; }
int  ear_audio_gain(void)      { return s_gain; }
void ear_audio_get_stats(ear_audio_stats_t *out) { *out = s_stats; }
