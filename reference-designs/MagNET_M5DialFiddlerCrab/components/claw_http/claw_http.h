#ifndef CLAW_HTTP_H
#define CLAW_HTTP_H
#include <stdbool.h>
#include "claw_mqtt.h"  // reuse claw_mqtt_msg_t for the parsed notify data

#ifdef __cplusplus
extern "C" {
#endif

// Callback for /notify endpoint -- same message structure as MQTT
typedef void (*claw_http_notify_cb_t)(const claw_mqtt_msg_t *msg, void *ctx);

// Callback for /status endpoint -- application provides JSON string
typedef const char *(*claw_http_status_cb_t)(void *ctx);

// Start HTTP server on port 80.
void claw_http_start(claw_http_notify_cb_t notify_cb,
                     claw_http_status_cb_t status_cb, void *ctx);

// Stop HTTP server.
void claw_http_stop(void);

#ifdef __cplusplus
}
#endif
#endif
