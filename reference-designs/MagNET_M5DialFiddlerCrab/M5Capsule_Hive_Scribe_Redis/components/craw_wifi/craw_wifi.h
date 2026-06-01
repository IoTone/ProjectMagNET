#ifndef CRAW_WIFI_H
#define CRAW_WIFI_H
#define CRAW_WIFI_VERSION "0.1.0"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CRAW_WIFI_EVENT_CONNECTED,
    CRAW_WIFI_EVENT_DISCONNECTED,
    CRAW_WIFI_EVENT_CONNECT_FAILED,
} craw_wifi_event_t;

typedef void (*craw_wifi_event_cb_t)(craw_wifi_event_t event, void *ctx);

// Initialize WiFi STA subsystem. Call once at boot.
void craw_wifi_init(const char *hostname, craw_wifi_event_cb_t cb, void *cb_ctx);

// Connect with the given SSID and password. Empty password = open network.
void craw_wifi_connect(const char *ssid, const char *pass);

// Disconnect from the current network.
void craw_wifi_disconnect(void);

// Check connection state.
bool craw_wifi_is_connected(void);

// Get IP address as string. Returns false if not connected.
bool craw_wifi_get_ip_str(char *buf, int len);

// Get the netif handle (for advanced queries).
void *craw_wifi_get_netif(void);

#ifdef __cplusplus
}
#endif
#endif
