/*
 * magnet_ota — the OTA supervisor.
 *
 * Native C with a thin Forth vocabulary over it, because ESPIDFORTH's ESP-IDF
 * engine is a STUB scheduled for replacement by the full ESP32forth v7.0.8.0.
 * Logic written against the stub's internals gets rewritten at that swap; logic
 * in C behind a handful of registered words survives it, and only the
 * registration shim moves.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_ERROR  = -1,
    OTA_NOOP   = 0,
    OTA_UPDATE = 1,
} ota_action_t;

typedef struct {
    long release_id;
    char version[32];
    char name[64];
    char sha256[72];
    char download_url[256];
    char signature_url[256];
    long expires_unix;
} ota_release_t;

esp_err_t    ota_init(void);

/*
 * Perform a check-in. ONE CALLER ONLY — the poll task in main.c owns this.
 * Anything else asks via ota_request_checkin().
 *
 * Two tasks calling it concurrently was not merely a race: the poll task
 * succeeded every 60 s while an identical console-issued request failed to TCP
 * connect, on a device answering pings in 6 ms. Rather than explain why two
 * esp_http_client users on this chip behave differently, there is now one.
 */
ota_action_t ota_checkin(void);

/* Ask the poll task to check in now. Returns immediately; watch ota_last_status()
 * or the [checkin] log line for the outcome. */
void         ota_request_checkin(void);

/* Blocks until nudged or until timeout_ms elapses. Poll task only. */
bool         ota_wait_checkin_request(uint32_t timeout_ms);

/* Valid only after ota_checkin() returned OTA_UPDATE. */
const ota_release_t *ota_pending(void);

/* Last outcome in words, for the screen and the console. */
const char  *ota_last_status(void);

/* Register the OTA-* words. The ONLY file that changes when the Forth engine
 * is swapped for the full ESP32forth. */
void         ota_register_forth_words(void);

#ifdef __cplusplus
}
#endif
