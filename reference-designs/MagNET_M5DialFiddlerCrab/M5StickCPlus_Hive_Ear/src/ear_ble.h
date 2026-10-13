#ifndef EAR_BLE_H
#define EAR_BLE_H

/* ear_ble — the Ear's BLE peripheral (NimBLE). The Ear is a BLE-only hive
 * accessory: everything a consumer needs to sync it into the hive comes
 * from standard SIG services, so nothing here is MagNET-specific on air.
 *
 *   GAP   name "MagNET-Ear-XXXX", appearance Generic Remote Control
 *   DIS   0x180A  manufacturer / model / serial (hive node id) / fw rev
 *   BAS   0x180F  battery level, read + notify
 *   ATVV  (E2)    Android TV Voice-over-BLE audio
 *
 * Security: LE Secure Connections, bonding, MITM via passkey shown on
 * the StickC display (IO cap DisplayOnly). Bonds persist in NVS.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EAR_BLE_EVT_PASSKEY,      /* arg = 6-digit passkey to display */
    EAR_BLE_EVT_PAIRED,       /* arg = 1 bonded+encrypted, 0 failed */
    EAR_BLE_EVT_CONNECTED,    /* arg = conn count */
    EAR_BLE_EVT_DISCONNECTED, /* arg = conn count */
    EAR_BLE_EVT_PAIR_REFUSED, /* new host tried to pair outside the pairing window */
} ear_ble_evt_t;

typedef void (*ear_ble_cb_t)(ear_ble_evt_t evt, uint32_t arg);

typedef struct {
    const char *model;       /* DIS model number */
    const char *fw_rev;      /* DIS firmware revision */
    const char *serial;      /* DIS serial — the hive node id */
} ear_ble_config_t;

/* Init controller + NimBLE host, register services, start advertising. */
int  ear_ble_init(const ear_ble_config_t *cfg, ear_ble_cb_t cb);

const char *ear_ble_name(void);       /* "MagNET-Ear-XXXX" */
const char *ear_ble_addr(void);       /* "aa:bb:cc:dd:ee:ff" (identity) */
int  ear_ble_conn_count(void);
bool ear_ble_advertising(void);
bool ear_ble_encrypted(void);         /* any connection encrypted+bonded */

/* Pairing window. New bonds are accepted only while it is open; with no
 * bonds and the window closed the Ear does not advertise at all (nothing to
 * reconnect to). Bonded hosts can always reconnect. Closing the window
 * aborts a pairing in progress. Closes itself after a successful bond. */
void ear_ble_pairing(bool open, int secs);
int  ear_ble_pairing_left_s(void);    /* 0 = closed */

/* Fast advertising (20-30 ms) while a talk press waits for a bonded host
 * to reconnect; off = normal 100-150 ms. */
void ear_ble_wake(bool on);

void ear_ble_battery_set(uint8_t pct);   /* notifies subscribers on change */
int  ear_ble_bond_count(void);
void ear_ble_unpair_all(void);

#ifdef __cplusplus
}
#endif
#endif
