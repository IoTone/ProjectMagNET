#ifndef EAR_ATVV_H
#define EAR_ATVV_H

/* ear_atvv — Android TV Voice-over-BLE (ATVV) v1.0, remote side.
 *
 * Service AB5E0001-5A21-4F05-BC7D-AF01F617B664
 *   TX  …0002  host→Ear write (GET_CAPS 0x0A, MIC_OPEN 0x0C, MIC_CLOSE 0x0D,
 *              MIC_EXTEND 0x0E). Requires an authenticated (MITM) bonded link.
 *   RX  …0003  Ear→host notify: headerless IMA-ADPCM frames.
 *   CTL …0004  Ear→host notify: AUDIO_STOP 0x00, AUDIO_START 0x04,
 *              AUDIO_SYNC 0x0A, CAPS_RESP 0x0B, MIC_OPEN_ERROR 0x0C.
 *
 * Interaction model: Hold-to-Talk (button A) + host-initiated MIC_OPEN.
 * Codec: ADPCM 16 kHz (0x02). Wire details: docs/Ear-ATVV-profile.md.
 */

#include <stdbool.h>
#include <stdint.h>
#include "host/ble_hs.h"

typedef enum { ATVV_IDLE, ATVV_READY, ATVV_LIVE } atvv_state_t;

typedef struct {
    atvv_state_t state;
    bool     waiting_host;  /* talk pressed, no host yet: buffering + fast adv */
    bool     capturing;     /* mic running (button held / MIC_OPEN) */
    bool     no_host;       /* recent talk press timed out without a host */
    uint16_t buffered_ms;   /* audio queued on the Ear, not yet sent */
    uint16_t conn_itvl_x100;/* connection interval, ms x100 (0 = none) */
    uint8_t  reason;        /* AUDIO_START reason of the live stream */
    uint32_t frames_sent;   /* this session */
    uint32_t frames_dropped;
    uint16_t frame_bytes;
    uint16_t mtu;
} atvv_status_t;

extern const struct ble_gatt_svc_def ear_atvv_svc;   /* embed in the GATT table */

int  ear_atvv_init(void);

/* GAP hooks (called from ear_ble's gap_event on the NimBLE host task). */
void ear_atvv_on_connect(uint16_t conn);
void ear_atvv_on_disconnect(uint16_t conn);
void ear_atvv_on_subscribe(uint16_t conn, uint16_t attr, bool notify);
void ear_atvv_on_mtu(uint16_t conn, uint16_t mtu);
void ear_atvv_on_enc(uint16_t conn);
void ear_atvv_on_conn_params(uint16_t conn, uint16_t itvl_1250us);

/* Thumb button: hold-to-talk. Works whenever the Ear is bonded: with no
 * host connected it buffers up to 3 s, advertises fast, and delivers the
 * utterance once a bonded host reconnects (or gives up after 6 s). */
void ear_atvv_button(bool down);
void ear_atvv_status(atvv_status_t *out);

#endif
