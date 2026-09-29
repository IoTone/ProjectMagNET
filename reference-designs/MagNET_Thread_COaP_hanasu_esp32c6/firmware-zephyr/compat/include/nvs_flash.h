/* nvs_flash.h — storage is brought up by main.c (settings_subsys_init). */
#ifndef MN_COMPAT_NVS_FLASH_H
#define MN_COMPAT_NVS_FLASH_H
#include "nvs.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
#ifdef __cplusplus
}
#endif
#endif
