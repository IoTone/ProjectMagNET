#ifndef CLAW_MQTT_H
#define CLAW_MQTT_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parsed MQTT message from the pipe-delimited format
typedef struct {
    int state;              // 0=IDLE, 2=WORKING, 3=NEED_INPUT, 5=FINISHED, 7=ERROR
    char model[32];         // e.g. "opus-4-6"
    int session_pct;        // -1 or 0-100
    int weekly_pct;         // -1 or 0-100
    uint32_t reset_epoch;   // 0 or unix timestamp
    char client_host[32];   // hostname of sending machine
    char session_id[40];    // extracted from topic — UUIDs are 36 chars
} claw_mqtt_msg_t;

typedef void (*claw_mqtt_msg_cb_t)(const claw_mqtt_msg_t *msg, void *ctx);
typedef void (*claw_mqtt_conn_cb_t)(bool connected, void *ctx);

// Initialize MQTT client. Does not connect until WiFi is up.
void claw_mqtt_init(const char *broker_uri, const char *base_topic,
                    claw_mqtt_msg_cb_t msg_cb, claw_mqtt_conn_cb_t conn_cb, void *ctx);

// Start the MQTT client (call after WiFi connects).
void claw_mqtt_start(void);

// Stop and destroy the client.
void claw_mqtt_stop(void);

// Re-initialize with a new broker URI (for mqtt-broker REPL command).
void claw_mqtt_set_broker(const char *broker_uri);

// Check connection state.
bool claw_mqtt_is_connected(void);

// Get the subscription topic string.
const char *claw_mqtt_get_topic(void);

#ifdef __cplusplus
}
#endif
#endif
