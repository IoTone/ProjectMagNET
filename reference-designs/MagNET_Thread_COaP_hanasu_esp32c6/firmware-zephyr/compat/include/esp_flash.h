/* esp_flash.h — only the SYSINFO size query. */
#ifndef MN_COMPAT_ESP_FLASH_H
#define MN_COMPAT_ESP_FLASH_H
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_flash_get_size(void *chip, uint32_t *out_size);
#ifdef __cplusplus
}
#endif
#endif
