#ifndef CRAW_WIFI_H
#define CRAW_WIFI_H
#define CRAW_WIFI_VERSION "0.1.0"
#include <stdbool.h>
#include <stdint.h>

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

// Connect to a specific BSSID for the given SSID. Bypasses band steering
// and mesh roaming during initial association — useful on ISP / consumer
// mesh routers where the AP keeps bouncing auth between nodes. PMF
// (Protected Management Frames) is explicitly disabled to avoid WPA2/WPA3
// transition-mode negotiation failures common to ESP32-C3 on IDF v5.3.
// `bssid` is a 6-byte MAC.
void craw_wifi_connect_bssid(const char *ssid, const char *pass,
                             const uint8_t bssid[6]);

// Convenience: scan, pick the strongest BSSID matching `ssid`, connect
// to it via craw_wifi_connect_bssid. Falls back to plain craw_wifi_connect
// if no matching BSSID is seen in the scan. Blocks for ~3-4 seconds on
// the scan. Recommended for ISP/consumer mesh networks; for "I know my AP"
// setups plain craw_wifi_connect is fine.
void craw_wifi_connect_best(const char *ssid, const char *pass);

// Disconnect from the current network.
void craw_wifi_disconnect(void);

// Check connection state.
bool craw_wifi_is_connected(void);

// Get IP address as string. Returns false if not connected.
bool craw_wifi_get_ip_str(char *buf, int len);

// Get the netif handle (for advanced queries).
void *craw_wifi_get_netif(void);

// ---- WiFi scan -----------------------------------------------------------
//
// Diagnostic: run an active scan across all 2.4 GHz channels and report
// nearby APs. Useful for "can't connect" debugging — confirms which SSIDs
// the device's radio actually sees, on which channel, at what signal
// strength. Often the answer is "the SSID I'm trying to connect to is
// only broadcasting on 5 GHz and the C3/C6 is 2.4 GHz only," which won't
// show up in any connect-side error.

typedef struct {
    char     ssid[33];     // null-terminated; empty if hidden
    int8_t   rssi;         // dBm; -40 ≈ same room, -80 ≈ far/walls
    uint8_t  channel;      // 1..14 (2.4 GHz)
    uint8_t  authmode;     // wifi_auth_mode_t value (see esp_wifi_types.h)
    uint8_t  bssid[6];     // AP MAC
} craw_wifi_scan_result_t;

// Run a scan and fill `results` with up to `max_results` entries, sorted
// by RSSI descending (strongest first). Returns the number actually
// written, or -1 on failure. Blocks for ~3-4 seconds while channels are
// swept.
//
// Safe to call before any connect attempt. If the WiFi driver hasn't
// been started yet this starts it first. Calling while a connection
// attempt is in flight will briefly preempt the attempt; the retry task
// resumes naturally afterwards.
int craw_wifi_scan(craw_wifi_scan_result_t *results, int max_results);

#ifdef __cplusplus
}
#endif
#endif
