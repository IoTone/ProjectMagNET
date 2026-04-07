/*
 * claw_speaker.h — Reusable speaker/buzzer module for MagNET Claw devices.
 *
 * Drives a piezo buzzer via ESP-IDF LEDC PWM.  The GPIO pin is set at init
 * time so the same code works on M5Dial (GPIO 3) and M5StickC Plus (GPIO 2).
 */
#ifndef CLAW_SPEAKER_H
#define CLAW_SPEAKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the LEDC timer + channel for tone generation on the given GPIO.
 * Must be called once before any other claw_speaker function.
 */
void claw_speaker_init(int gpio_pin);

/**
 * Start a non-blocking tone.  The tone plays for @p duration_ms milliseconds
 * and is silenced automatically by claw_speaker_update().
 */
void claw_speaker_tone(uint16_t freq, uint16_t duration_ms);

/**
 * Main-loop tick.  Stops a tone whose duration has elapsed and fires any
 * scheduled second tone (two-tone chime pattern).
 */
void claw_speaker_update(void);

/** Enable or disable sound globally.  Chime helpers are silent when disabled. */
void claw_speaker_set_sound_enabled(bool enabled);

/** Return current sound-enabled state. */
bool claw_speaker_is_sound_enabled(void);

/* ---- State chimes ---- */

/** Short rising beep — device is working. */
void claw_speaker_chime_working(void);

/** Two-tone ascending — task finished. */
void claw_speaker_chime_finished(void);

/** Single high beep — user input required. */
void claw_speaker_chime_need_input(void);

/** Two-tone descending — error occurred. */
void claw_speaker_chime_error(void);

#ifdef __cplusplus
}
#endif

#endif /* CLAW_SPEAKER_H */
