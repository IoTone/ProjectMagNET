#ifndef CRAW_BLE_PROVISION_H
#define CRAW_BLE_PROVISION_H
#define CRAW_BLE_PROVISION_VERSION "0.1.0"

// craw_ble_provision — BLE GATT provisioning service for MagNET hive nodes.
//
// Satisfies R3 + R4 of the MagNET hive design (see MagNET_M5DialFiddlerCrab
// README.md): on boot an unconfigured node advertises as
// "MagNET-biologic-<MAC4>"; a connected controller writes SSID / pass and a
// commit trigger; the node then attempts WiFi join and publishes its IP back
// over a notify characteristic.
//
// Usage:
//   craw_ble_provision_config_t cfg = {
//       .name_prefix = "MagNET-biologic",  // or NULL for default
//       .role        = "spawn",             // optional, surfaced in device_info
//   };
//   craw_ble_provision_init(&cfg, on_prov_event, NULL);
//
// On CRAW_BLE_PROV_COMMIT_REQUESTED the callback should pull ssid/pass from
// the provided pointers, save them (e.g. via craw_nvs_save_wifi_creds), call
// craw_wifi_connect(), then report transitions back via
// craw_ble_provision_set_status() and craw_ble_provision_set_ip().

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CRAW_BLE_PROV_IDLE             = 0,
    CRAW_BLE_PROV_CREDS_RECEIVED   = 1, // ssid and/or pass written, no commit yet
    CRAW_BLE_PROV_COMMIT_REQUESTED = 2, // commit trigger written — app should connect
    CRAW_BLE_PROV_CONNECTING       = 3, // app says "attempting WiFi join"
    CRAW_BLE_PROV_CONNECTED        = 4, // app says "WiFi up"
    CRAW_BLE_PROV_FAILED           = 5, // app says "WiFi failed"
} craw_ble_prov_state_t;

typedef struct {
    const char *name_prefix;  // default "MagNET-biologic" when NULL
    const char *role;         // optional — appears in device_info JSON
} craw_ble_provision_config_t;

typedef void (*craw_ble_provision_cb_t)(craw_ble_prov_state_t state,
                                        const char *ssid,
                                        const char *pass,
                                        void *ctx);

// Init NimBLE, register the provisioning GATT service, start advertising.
// Safe to call once at boot, after craw_nvs_init_flash().
void craw_ble_provision_init(const craw_ble_provision_config_t *cfg,
                             craw_ble_provision_cb_t cb,
                             void *cb_ctx);

// Inform BLE clients of the current provisioning state via notify.
void craw_ble_provision_set_status(craw_ble_prov_state_t state);

// Publish the node's IP (or "N/A"). Sends a notify on the IP characteristic.
void craw_ble_provision_set_ip(const char *ip_str);

// Restart advertising manually. The service restarts advertising on
// disconnect automatically — this is only needed for forced re-advertise.
void craw_ble_provision_advertise(void);

// Stop advertising. Useful once the node is provisioned and no longer needs
// to be discoverable — on some boards (notably M5Atom Echo) continuous BLE
// beacons couple into the audio amp and cause audible clicks.
void craw_ble_provision_stop_advertising(void);

// Fully tear down NimBLE + the BT controller to free ~50 KB back to the heap.
// Call once after WiFi provisioning completes and BLE is no longer needed.
// BLE cannot be re-initialized in this process after deinit — a reboot is
// required to resume advertising (e.g. via `esp_restart()` from prov-reset).
void craw_ble_provision_deinit(void);

// Derived device name ("MagNET-biologic-a1b2"). Pointer to internal buffer.
const char *craw_ble_provision_device_name(void);

#ifdef __cplusplus
}
#endif
#endif
