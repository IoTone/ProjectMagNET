#ifndef CRAW_HTTP_H
#define CRAW_HTTP_H
#include <stdbool.h>
#include "craw_mqtt.h"  // reuse craw_mqtt_msg_t for the parsed notify data

#ifdef __cplusplus
extern "C" {
#endif

// Callback for /notify endpoint -- same message structure as MQTT
typedef void (*craw_http_notify_cb_t)(const craw_mqtt_msg_t *msg, void *ctx);

// Callback for /status endpoint -- application provides JSON string
typedef const char *(*craw_http_status_cb_t)(void *ctx);

// Start HTTP server on port 80.
void craw_http_start(craw_http_notify_cb_t notify_cb,
                     craw_http_status_cb_t status_cb, void *ctx);

// Stop HTTP server.
void craw_http_stop(void);

#ifdef __cplusplus
}
#endif
#endif
