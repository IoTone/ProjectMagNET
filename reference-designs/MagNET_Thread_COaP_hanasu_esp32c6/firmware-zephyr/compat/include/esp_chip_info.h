/* esp_chip_info.h — ESP chip enum kept so switch statements compile; the MG24
 * reports CHIP_EFR32MG24, which ESP-only code prints as "Unknown". */
#ifndef MN_COMPAT_ESP_CHIP_INFO_H
#define MN_COMPAT_ESP_CHIP_INFO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    CHIP_ESP32 = 1, CHIP_ESP32S2 = 2, CHIP_ESP32S3 = 9, CHIP_ESP32C3 = 5,
    CHIP_ESP32C6 = 13, CHIP_ESP32H2 = 16, CHIP_EFR32MG24 = 0x2400,
} esp_chip_model_t;
/* feature bits: same values as IDF */
#define CHIP_FEATURE_EMB_FLASH   (1u << 0)
#define CHIP_FEATURE_WIFI_BGN    (1u << 1)
#define CHIP_FEATURE_BLE         (1u << 4)
#define CHIP_FEATURE_BT          (1u << 5)
#define CHIP_FEATURE_IEEE802154  (1u << 6)
#define CHIP_FEATURE_EMB_PSRAM   (1u << 7)
typedef struct {
    esp_chip_model_t model;
    uint32_t features;
    uint16_t revision;
    uint8_t  cores;
} esp_chip_info_t;
void esp_chip_info(esp_chip_info_t *out);
#ifdef __cplusplus
}
#endif
#endif
