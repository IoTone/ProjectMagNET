/* esp_err.h — IDF error codes the MagNET sources compare against (Zephyr shim). */
#ifndef MN_COMPAT_ESP_ERR_H
#define MN_COMPAT_ESP_ERR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int esp_err_t;
#define ESP_OK                          0
#define ESP_FAIL                        -1
#define ESP_ERR_NO_MEM                  0x101
#define ESP_ERR_INVALID_ARG             0x102
#define ESP_ERR_INVALID_STATE           0x103
#define ESP_ERR_INVALID_SIZE            0x104
#define ESP_ERR_NOT_FOUND               0x105
#define ESP_ERR_NVS_NOT_FOUND           0x1102
#define ESP_ERR_NVS_INVALID_LENGTH      0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES       0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND   0x1110
void mn_compat_panic(const char *file, int line);
#define ESP_ERROR_CHECK(x) do { if ((esp_err_t)(x) != ESP_OK) mn_compat_panic(__FILE__, __LINE__); } while (0)
static inline const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_ERR"; }
#ifdef __cplusplus
}
#endif
#endif
