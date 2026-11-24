#ifndef CRAW_MQTT_H
#define CRAW_MQTT_H
#define CRAW_MQTT_VERSION "0.3.0"
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
} craw_mqtt_msg_t;

typedef void (*craw_mqtt_msg_cb_t)(const craw_mqtt_msg_t *msg, void *ctx);
typedef void (*craw_mqtt_conn_cb_t)(bool connected, void *ctx);

// Initialize MQTT client. Does not connect until WiFi is up.
// broker_uri may be mqtt://, mqtts://, ws:// or wss://; the TLS ones are
// verified against ESP-IDF's root certificate bundle.
//
// base_topic semantics:
//   - non-empty: subscriber + publisher; auto-subscribes "<base_topic>/#"
//                on connect; pipe-delimited messages dispatched to msg_cb.
//   - NULL or "":publisher-only; no automatic subscribe. Used by MagNET
//                bridge nodes (Capsule scribe) that only publish.
void craw_mqtt_init(const char *broker_uri, const char *base_topic,
                    craw_mqtt_msg_cb_t msg_cb, craw_mqtt_conn_cb_t conn_cb, void *ctx);

// Publish a NUL-terminated payload to a topic. Returns esp-mqtt's message
// id (>=0) on success, -1 if the client isn't initialized. QoS 0 /
// retain=false is typical for sensor telemetry.
int craw_mqtt_publish(const char *topic, const char *payload,
                      int qos, bool retain);

// Optional last will: the broker publishes `payload` to `topic` if this
// client drops off without a clean disconnect (power cut, WiFi lost).
// keepalive_s sets how fast the broker notices (it waits 1.5x keepalive);
// 0 keeps esp-mqtt's default of 120 s. Call before craw_mqtt_start().
void craw_mqtt_set_lwt(const char *topic, const char *payload, int qos,
                       bool retain, int keepalive_s);

// Start the MQTT client (call after WiFi connects).
void craw_mqtt_start(void);

// Stop and destroy the client.
void craw_mqtt_stop(void);

// Re-initialize with a new broker URI (for mqtt-broker REPL command).
void craw_mqtt_set_broker(const char *broker_uri);

// Check connection state.
bool craw_mqtt_is_connected(void);

// Get the subscription topic string.
const char *craw_mqtt_get_topic(void);

#ifdef __cplusplus
}
#endif
#endif
