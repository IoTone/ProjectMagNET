/* esp_idf_version.h — reports the Zephyr kernel version instead of IDF's. */
#ifndef MN_COMPAT_ESP_IDF_VERSION_H
#define MN_COMPAT_ESP_IDF_VERSION_H
#ifdef __cplusplus
extern "C" {
#endif
const char *esp_get_idf_version(void);
#ifdef __cplusplus
}
#endif
#endif
