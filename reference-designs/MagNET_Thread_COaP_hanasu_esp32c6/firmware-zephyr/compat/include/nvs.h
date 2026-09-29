/* nvs.h — IDF NVS on top of Zephyr settings (key "mn/<namespace>/<key>").
 *
 * Semantics kept from IDF because magnet_core.c depends on them:
 *   - get_str/get_blob with out==NULL report the required size in *len;
 *     a too-small buffer is ESP_ERR_NVS_INVALID_LENGTH; str sizes count NUL.
 *   - a missing key is ESP_ERR_NVS_NOT_FOUND and leaves *out untouched.
 * Writes go to flash immediately, so nvs_commit() is a no-op success. */
#ifndef MN_COMPAT_NVS_H
#define MN_COMPAT_NVS_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
typedef struct mn_nvs *nvs_handle_t;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v);

/* ---- iteration (IDF 5 API, used by the role-bundle engine) ----
 * Settings stores untyped blobs, so the type filter cannot be honoured: every
 * key in the namespace is returned and entries report the requested type.
 * The only caller ("role_bundle" namespace) stores nothing but strings. */
#define NVS_DEFAULT_PART_NAME "nvs"
#define NVS_KEY_NAME_MAX_SIZE 16
typedef enum {
    NVS_TYPE_U8 = 0x01, NVS_TYPE_U32 = 0x04, NVS_TYPE_STR = 0x21,
    NVS_TYPE_BLOB = 0x42, NVS_TYPE_ANY = 0xff,
} nvs_type_t;
typedef struct {
    char namespace_name[NVS_KEY_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
} nvs_entry_info_t;
typedef struct mn_nvs_iter *nvs_iterator_t;

/* ESP_ERR_NVS_NOT_FOUND (and *it = NULL) when the namespace is empty */
esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *it);
/* advances; at the end releases the iterator, sets *it = NULL, NOT_FOUND */
esp_err_t nvs_entry_next(nvs_iterator_t *it);
esp_err_t nvs_entry_info(const nvs_iterator_t it, nvs_entry_info_t *out);
void      nvs_release_iterator(nvs_iterator_t it);
#ifdef __cplusplus
}
#endif
#endif
