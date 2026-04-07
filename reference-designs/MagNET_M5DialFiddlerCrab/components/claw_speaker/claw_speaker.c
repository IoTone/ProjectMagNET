/*
 * claw_speaker.c — ESP-IDF LEDC-based buzzer driver for MagNET Claw devices.
 */
#include "claw_speaker.h"

#include "driver/ledc.h"
#include "esp_timer.h"

/* ---- LEDC constants ---- */
#define SPEAKER_LEDC_CHANNEL  LEDC_CHANNEL_0
#define SPEAKER_LEDC_TIMER    LEDC_TIMER_0

/* ---- Internal state ---- */
static uint8_t  speaker_volume   = 180;
static uint32_t speaker_stop_at  = 0;    /* millis timestamp to silence tone */
static bool     sound_enabled    = false;

/* Two-tone scheduling */
static uint32_t tone2_at   = 0;
static uint16_t tone2_freq = 0;
static uint16_t tone2_dur  = 0;

/* ---- Helpers ---- */
static uint32_t millis_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ---- Public API ---- */

void claw_speaker_init(int gpio_pin)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = SPEAKER_LEDC_TIMER,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t chan_conf = {
        .gpio_num   = gpio_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = SPEAKER_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = SPEAKER_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&chan_conf);
}

void claw_speaker_tone(uint16_t freq, uint16_t duration_ms)
{
    if (freq == 0) return;
    ledc_set_freq(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_TIMER, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL, speaker_volume / 2);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL);
    speaker_stop_at = millis_now() + duration_ms;
}

void claw_speaker_update(void)
{
    uint32_t now = millis_now();

    /* Auto-stop current tone when its duration expires */
    if (speaker_stop_at && now >= speaker_stop_at) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL);
        speaker_stop_at = 0;
    }

    /* Fire scheduled second tone */
    if (tone2_at && now >= tone2_at) {
        uint16_t f = tone2_freq;
        uint16_t d = tone2_dur;
        tone2_at   = 0;
        tone2_freq = 0;
        tone2_dur  = 0;
        claw_speaker_tone(f, d);
    }
}

void claw_speaker_set_sound_enabled(bool enabled)
{
    sound_enabled = enabled;
}

bool claw_speaker_is_sound_enabled(void)
{
    return sound_enabled;
}

/* ---- State chimes ---- */

void claw_speaker_chime_working(void)
{
    if (!sound_enabled) return;
    claw_speaker_tone(800, 50);
}

void claw_speaker_chime_finished(void)
{
    if (!sound_enabled) return;
    claw_speaker_tone(1200, 80);
    tone2_at   = millis_now() + 120;
    tone2_freq = 1800;
    tone2_dur  = 100;
}

void claw_speaker_chime_need_input(void)
{
    if (!sound_enabled) return;
    claw_speaker_tone(2000, 100);
}

void claw_speaker_chime_error(void)
{
    if (!sound_enabled) return;
    claw_speaker_tone(1000, 80);
    tone2_at   = millis_now() + 120;
    tone2_freq = 600;
    tone2_dur  = 100;
}
