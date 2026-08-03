/*
 * magnet_transport — how a check-in request reaches the server.
 *
 * THE POINT OF THIS FILE is that D2 must not call esp_http_client directly.
 * Four transports are wanted eventually (plan section 5): direct IP,
 * mDNS-discovered, BLE via a phone proxy, and USB serial in the style of the
 * meshcore web flasher. They are not four peers:
 *
 *   - mDNS is DISCOVERY, not transport. It answers "what is the base URL"; the
 *     bytes still travel over IP. Making it a transport would duplicate the
 *     whole HTTP path in order to change one string.
 *   - BLE and serial are the SAME transport over different pipes. In both the
 *     device has no IP and cannot speak HTTP; it emits a framed request and
 *     something else relays it. They differ only in read/write.
 *
 * So: one vtable, transport_ip now, transport_relay later. Retrofitting this
 * boundary afterwards is what turns R3/R4 into rewrites, which is why it exists
 * before there is a second implementation to justify it.
 */
#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct magnet_transport {
    const char *name;
    esp_err_t (*open)(struct magnet_transport *t);
    /*
     * One request. Returns the HTTP status (200, 401, ...) or a NEGATIVE value
     * on transport failure. Those are genuinely different things, and
     * collapsing them loses the distinction between "the server said no" and
     * "we never reached the server" — exactly what someone staring at a stuck
     * device needs to tell apart.
     */
    int (*request)(struct magnet_transport *t,
                   const char *method, const char *path,
                   const char *body,
                   char *resp, size_t resp_cap, size_t *resp_len);
    void (*close)(struct magnet_transport *t);
    void *ctx;
} magnet_transport_t;

/* Direct IP. Base URL and bearer token both come from magnet_cfg. mDNS
 * discovery later replaces only where the URL comes from, nothing else. */
magnet_transport_t *transport_ip(void);

#ifdef __cplusplus
}
#endif
