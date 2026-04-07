#ifndef CLAW_WIFI_H
#define CLAW_WIFI_H
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_WIFI_EVENT_CONNECTED,
    CLAW_WIFI_EVENT_DISCONNECTED,
    CLAW_WIFI_EVENT_CONNECT_FAILED,
} claw_wifi_event_t;

typedef void (*claw_wifi_event_cb_t)(claw_wifi_event_t event, void *ctx);

// Initialize WiFi STA subsystem. Call once at boot.
void claw_wifi_init(const char *hostname, claw_wifi_event_cb_t cb, void *cb_ctx);

// Connect with the given SSID and password. Empty password = open network.
void claw_wifi_connect(const char *ssid, const char *pass);

// Disconnect from the current network.
void claw_wifi_disconnect(void);

// Check connection state.
bool claw_wifi_is_connected(void);

// Get IP address as string. Returns false if not connected.
bool claw_wifi_get_ip_str(char *buf, int len);

// Get the netif handle (for advanced queries).
void *claw_wifi_get_netif(void);

#ifdef __cplusplus
}
#endif
#endif
