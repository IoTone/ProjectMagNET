/*
 * http_strip.h - HTTP API for the UC2 neopixel actuator
 *
 * GET/POST /api/v1/actuator/neopixel  (+ wildcard OPTIONS for CORS).
 * Matches the contract the in-XR UC2 actuator panel POSTs.
 */
#ifndef HTTP_STRIP_H
#define HTTP_STRIP_H

#include "esp_err.h"

esp_err_t http_strip_start(void);
void      http_strip_stop(void);

#endif /* HTTP_STRIP_H */
