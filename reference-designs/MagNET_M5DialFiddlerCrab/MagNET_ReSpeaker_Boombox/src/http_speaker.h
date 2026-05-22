/*
 * http_speaker.h - UC2 speaker actuator HTTP API
 *
 * POST /api/v1/actuator/speaker/play { sound_id } - play a named sound
 *   (chime, doorbell, alert, notify, warn, error, sunrise, siren, yelp,
 *    nee-naw, air-raid)
 * GET  /api/v1/actuator/speaker      - report device capabilities
 * OPTIONS (wildcard)                 - CORS preflight
 */
#ifndef HTTP_SPEAKER_H
#define HTTP_SPEAKER_H

#include "esp_err.h"

esp_err_t http_speaker_start(void);
void      http_speaker_stop(void);

#endif /* HTTP_SPEAKER_H */
