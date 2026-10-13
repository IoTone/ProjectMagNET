/* esp_random.h — backed by the MG24 Secure Engine TRNG (sys_csrand_get). */
#ifndef MN_COMPAT_ESP_RANDOM_H
#define MN_COMPAT_ESP_RANDOM_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_random(void);
void     esp_fill_random(void *buf, size_t len);
#ifdef __cplusplus
}
#endif
#endif
