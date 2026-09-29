/* esp_system.h — restart, reset reason, heap and version queries. */
#ifndef MN_COMPAT_ESP_SYSTEM_H
#define MN_COMPAT_ESP_SYSTEM_H
#include <stdint.h>
#include "esp_err.h"
#include "esp_idf_version.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int esp_reset_reason_t;         /* raw Zephyr hwinfo reset-cause bits */
void               esp_restart(void) __attribute__((noreturn));
esp_reset_reason_t esp_reset_reason(void);
uint32_t           esp_get_free_heap_size(void);
#ifdef __cplusplus
}
#endif
#endif
