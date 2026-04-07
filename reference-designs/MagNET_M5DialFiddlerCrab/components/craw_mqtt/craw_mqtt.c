#include "craw_mqtt.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "craw_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;

static char s_broker_uri[128];
static char s_base_topic[128];
static char s_sub_topic[140];   // base_topic + "/#"

static craw_mqtt_msg_cb_t  s_msg_cb  = NULL;
static craw_mqtt_conn_cb_t s_conn_cb = NULL;
static void *s_ctx = NULL;

// ---------------------------------------------------------------------------
// Parse pipe-delimited message into craw_mqtt_msg_t.
// Format: state|model|session_pct|weekly_pct|reset_epoch|client_host
// Also supports plain integer for backward compatibility.
// ---------------------------------------------------------------------------
static void parse_message(const char *data, int data_len,
                          const char *topic, int topic_len,
                          craw_mqtt_msg_t *out) {
    memset(out, 0, sizeof(*out));
    out->session_pct = -1;
    out->weekly_pct  = -1;

    // Extract session_id from topic: if topic has more segments than base,
    // the last segment is the session_id.
    if (topic && topic_len > 0) {
        int base_len = (int)strlen(s_base_topic);
        if (topic_len > base_len + 1 && topic[base_len] == '/') {
            const char *session_start = topic + base_len + 1;
            int session_len = topic_len - base_len - 1;
            if (session_len > 0 && session_len < (int)sizeof(out->session_id)) {
                memcpy(out->session_id, session_start, session_len);
                out->session_id[session_len] = '\0';
            }
        }
    }

    // Copy payload into a null-terminated buffer
    char buf[200] = {0};
    int copy_len = data_len < (int)sizeof(buf) - 1 ? data_len : (int)sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    if (strchr(buf, '|') != NULL) {
        // Pipe-delimited extended format
        char model[32] = {0};
        int sess = -1, wkly = -1;
        unsigned int reset_e = 0;
        char host[32] = {0};

        int parsed = sscanf(buf, "%d|%31[^|]|%d|%d|%u|%31[^|]",
                            &out->state, model, &sess, &wkly, &reset_e, host);
        if (parsed >= 2 && strlen(model) > 0) {
            strncpy(out->model, model, sizeof(out->model) - 1);
        }
        if (parsed >= 3) out->session_pct = sess;
        if (parsed >= 4) out->weekly_pct  = wkly;
        if (parsed >= 5) out->reset_epoch = reset_e;
        if (parsed >= 6 && strlen(host) > 0) {
            strncpy(out->client_host, host, sizeof(out->client_host) - 1);
        }

        ESP_LOGI(TAG, "msg: state=%d model=%s sess=%d wkly=%d host=%s",
                 out->state, out->model, out->session_pct,
                 out->weekly_pct, out->client_host);
    } else {
        // Backward compat: plain integer
        out->state = atoi(buf);
        ESP_LOGI(TAG, "msg: state=%d (plain)", out->state);
    }
}

// ---------------------------------------------------------------------------
// MQTT event handler
// ---------------------------------------------------------------------------
static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            esp_mqtt_client_subscribe(s_client, s_sub_topic, 1);
            ESP_LOGI(TAG, "connected, subscribed to: %s", s_sub_topic);
            if (s_conn_cb) s_conn_cb(true, s_ctx);
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "disconnected");
            if (s_conn_cb) s_conn_cb(false, s_ctx);
            break;

        case MQTT_EVENT_DATA:
            if (event->data_len > 0 && event->data_len < 200) {
                craw_mqtt_msg_t msg;
                parse_message(event->data, event->data_len,
                              event->topic, event->topic_len, &msg);
                if (s_msg_cb) s_msg_cb(&msg, s_ctx);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "error");
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void craw_mqtt_init(const char *broker_uri, const char *base_topic,
                    craw_mqtt_msg_cb_t msg_cb, craw_mqtt_conn_cb_t conn_cb, void *ctx) {
    strncpy(s_broker_uri, broker_uri, sizeof(s_broker_uri) - 1);
    s_broker_uri[sizeof(s_broker_uri) - 1] = '\0';

    strncpy(s_base_topic, base_topic, sizeof(s_base_topic) - 1);
    s_base_topic[sizeof(s_base_topic) - 1] = '\0';

    snprintf(s_sub_topic, sizeof(s_sub_topic), "%s/#", s_base_topic);

    s_msg_cb  = msg_cb;
    s_conn_cb = conn_cb;
    s_ctx     = ctx;
}

void craw_mqtt_start(void) {
    if (s_client) return;

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = s_broker_uri;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "started, broker: %s", s_broker_uri);
}

void craw_mqtt_stop(void) {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
}

void craw_mqtt_set_broker(const char *broker_uri) {
    craw_mqtt_stop();
    strncpy(s_broker_uri, broker_uri, sizeof(s_broker_uri) - 1);
    s_broker_uri[sizeof(s_broker_uri) - 1] = '\0';
    craw_mqtt_start();
}

bool craw_mqtt_is_connected(void) {
    return s_connected;
}

const char *craw_mqtt_get_topic(void) {
    return s_base_topic;
}
