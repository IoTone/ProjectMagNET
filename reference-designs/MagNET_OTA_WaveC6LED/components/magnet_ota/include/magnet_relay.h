/*
 * magnet_relay — the relay transport and the pipes that carry it.
 *
 * Plan section 5.1: BLE and USB serial are the SAME transport over different
 * pipes. The device has no IP on either and cannot speak HTTP; it emits one
 * framed request and something on the other end — the contributor phone app
 * over GATT, a WebSerial page over USB — performs the HTTP call and frames the
 * answer back. This header is that boundary:
 *
 *   transport_relay()   the magnet_transport_t the supervisor uses, exactly
 *                       like transport_ip(): open / request / close.
 *   relay_pipe_t        what a pipe must provide: is anyone attached, how big
 *                       a frame may be, and write one frame.
 *   relay_rx()          what a pipe calls when a frame arrives.
 *
 * The framing itself is in relay_frame.h and is shared byte-for-byte with the
 * Flutter proxy (contributor/lib/relay/relay_frame.dart) and the WebSerial
 * page. Change one, change all three — there is a test on each side.
 *
 * WHAT CROSSES THE PIPE, deliberately (plan section 5.3, decision (a)): the
 * request carries the dvc_ bearer token in clear text and the proxy attaches
 * it to the HTTP call. The proxy is operator-controlled — your phone, your
 * browser, during provisioning — and this is written down here rather than
 * discovered later. (b), the device-authenticated envelope where the token
 * never crosses, is a server-side change (S3) the frame version byte can
 * express when it lands.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "magnet_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct relay_pipe {
    const char *name;                       /* "ble", "serial" */
    bool   (*attached)(void);               /* a proxy is on the other end */
    size_t (*max_frame)(void);              /* largest single write, bytes */
    /* Write ONE complete frame. Blocking; 0 on success, negative on failure.
     * Called from the supervisor's task, never from the pipe's own RX path. */
    int    (*write)(const uint8_t *frame, size_t len);
} relay_pipe_t;

/* Pipes register once at init. Two is the design (BLE, serial); a third would
 * be a sign the decomposition in section 5.1 has drifted. */
void relay_register_pipe(const relay_pipe_t *pipe);

/* True when some pipe has a proxy attached — the supervisor's cue to prefer
 * the relay over transport_ip. The pipe that is attached is the one used. */
bool relay_available(void);
const char *relay_pipe_name(void);          /* attached pipe's name, or "none" */

/* A pipe delivers an inbound frame here. Any task; must not block. The
 * transport validates, reassembles and wakes the waiting request. */
void relay_rx(const uint8_t *frame, size_t len);

/* The transport. Same lifetime model as transport_ip(): a static singleton. */
magnet_transport_t *transport_relay(void);

/* The BLE pipe (pipe_ble.c). Compiled to stubs returning ESP_ERR_NOT_SUPPORTED
 * when CONFIG_BT_ENABLED is off, so main links either way. */
esp_err_t   pipe_ble_init(void);
const char *pipe_ble_name(void);
bool        pipe_ble_connected(void);       /* central connected (attached or not) */

/* The serial pipe (pipe_serial.c): rides the console as "!R<base64>" lines.
 * PRINT is the console's writer; the console feeds any line starting with
 * '!' to pipe_serial_line() and drops it when that returns true. */
void pipe_serial_init(void (*print)(const char *));
bool pipe_serial_line(const char *line);
bool pipe_serial_attached(void);

/* Counters for the console's `relay` command and the status screen. */
typedef struct {
    uint32_t requests, responses, frames_out, frames_in, bad_frames, timeouts;
} relay_stats_t;
const relay_stats_t *relay_stats(void);

#ifdef __cplusplus
}
#endif
