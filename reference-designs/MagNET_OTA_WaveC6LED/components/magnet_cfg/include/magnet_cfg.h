/*
 * magnet_cfg — the device's persistent identity and where it checks in.
 *
 * Deliberately a flat key/value store rather than lifting craw_nvs: that is a
 * 407-line WiFi PROFILE manager (named profiles, lists, migration), and this
 * device needs one set of credentials plus three OTA fields. Borrowing the
 * bigger abstraction would mean carrying a profile model that nothing here has
 * a use for.
 *
 * NOTHING IS COMPILED IN. Credentials and the device token arrive over the
 * console and live in NVS. A repo is the wrong place for either, and a token
 * baked into a firmware image is a token you cannot rotate.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAX 128

esp_err_t cfg_init(void);

/* Return false when the key is unset, leaving *out an empty string — callers
 * distinguish "not provisioned" from "provisioned empty". */
bool cfg_get(const char *key, char *out, size_t out_len);
esp_err_t cfg_set(const char *key, const char *value);
esp_err_t cfg_erase(const char *key);

/* Known keys. Strings, not an enum, so a Forth word can name one directly. */
#define CFG_WIFI_SSID  "wifi_ssid"
#define CFG_WIFI_PASS  "wifi_pass"
#define CFG_SERVER_URL "server_url"
#define CFG_DEV_TOKEN  "dev_token"

/* True when everything needed to reach the server is present. */
bool cfg_provisioned(void);

#ifdef __cplusplus
}
#endif
