/* esp_mac.h — 6 bytes of the factory EUI-64 (Zephyr hwinfo device id). */
#ifndef MN_COMPAT_ESP_MAC_H
#define MN_COMPAT_ESP_MAC_H
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_efuse_mac_get_default(uint8_t mac[6]);
#ifdef __cplusplus
}
#endif
#endif
