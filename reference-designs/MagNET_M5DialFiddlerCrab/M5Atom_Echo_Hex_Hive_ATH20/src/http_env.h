/*
 * http_env.h - UC2 environment sensor HTTP API
 *
 * GET /api/v1/sensor/environment  -> { temperature_c, humidity_pct, ts_ms, hostname, ... }
 * GET /api/v1/sensor/temperature  -> { value_c, ts_ms }
 * GET /api/v1/sensor/humidity     -> { value_pct, ts_ms }
 * OPTIONS (wildcard)              -> CORS preflight
 */
#ifndef HTTP_ENV_H
#define HTTP_ENV_H

#include "esp_err.h"

esp_err_t http_env_start(void);
void      http_env_stop(void);

#endif /* HTTP_ENV_H */
