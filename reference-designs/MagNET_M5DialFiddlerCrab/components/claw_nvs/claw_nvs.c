#include "claw_nvs.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "claw_nvs";

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static char s_active_profile[CLAW_PROFILE_NAME_MAX + 1] = CLAW_DEFAULT_PROFILE_NAME;
static char s_profile_names[CLAW_PROFILE_MAX_COUNT][CLAW_PROFILE_NAME_MAX + 1];
static int  s_profile_count = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Build NVS key "s_<name>" or "p_<name>" into out (min 15 bytes)
static void build_profile_key(char prefix, const char *name, char *out)
{
    out[0] = prefix;
    out[1] = '_';
    int i = 0;
    while (i < CLAW_PROFILE_NAME_MAX && name[i]) {
        out[i + 2] = name[i];
        i++;
    }
    out[i + 2] = '\0';
}

// Lowercase a string in-place
static void str_tolower(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
    }
}

// Parse comma-separated profile list into s_profile_names[]
static void profile_list_parse(const char *list)
{
    s_profile_count = 0;
    if (!list || !*list) return;
    const char *p = list;
    while (*p && s_profile_count < CLAW_PROFILE_MAX_COUNT) {
        // Skip leading commas/whitespace
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ',' && n < CLAW_PROFILE_NAME_MAX) {
            s_profile_names[s_profile_count][n++] = *p++;
        }
        s_profile_names[s_profile_count][n] = '\0';
        // Skip any overflow chars
        while (*p && *p != ',') p++;
        if (n > 0) s_profile_count++;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
void claw_nvs_init_flash(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

// ---------------------------------------------------------------------------
// Profile management — public API
// ---------------------------------------------------------------------------
const char *claw_nvs_active_profile(void)
{
    return s_active_profile;
}

const char (*claw_nvs_profile_list(int *count))[CLAW_PROFILE_NAME_MAX + 1]
{
    if (count) *count = s_profile_count;
    return s_profile_names;
}

int claw_nvs_profile_count(void)
{
    return s_profile_count;
}

bool claw_nvs_profile_name_valid(const char *name)
{
    if (!name) return false;
    int len = (int)strlen(name);
    if (len < 1 || len > CLAW_PROFILE_NAME_MAX) return false;
    for (int i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

int claw_nvs_profile_find(const char *name)
{
    for (int i = 0; i < s_profile_count; i++) {
        if (strcmp(s_profile_names[i], name) == 0) return i;
    }
    return -1;
}

bool claw_nvs_profile_add(const char *name)
{
    if (s_profile_count >= CLAW_PROFILE_MAX_COUNT) return false;
    if (claw_nvs_profile_find(name) >= 0) return false;
    strncpy(s_profile_names[s_profile_count], name, CLAW_PROFILE_NAME_MAX);
    s_profile_names[s_profile_count][CLAW_PROFILE_NAME_MAX] = '\0';
    s_profile_count++;
    return true;
}

void claw_nvs_profile_remove(const char *name)
{
    int idx = claw_nvs_profile_find(name);
    if (idx < 0) return;
    for (int i = idx; i < s_profile_count - 1; i++) {
        strncpy(s_profile_names[i], s_profile_names[i + 1], CLAW_PROFILE_NAME_MAX + 1);
    }
    s_profile_count--;
    s_profile_names[s_profile_count][0] = '\0';
}

void claw_nvs_profile_list_save(void)
{
    char buf[CLAW_PROFILE_MAX_COUNT * (CLAW_PROFILE_NAME_MAX + 1) + 4] = {0};
    int pos = 0;
    for (int i = 0; i < s_profile_count; i++) {
        if (i > 0 && pos < (int)sizeof(buf) - 1) buf[pos++] = ',';
        int n = (int)strlen(s_profile_names[i]);
        if (pos + n >= (int)sizeof(buf)) break;
        memcpy(buf + pos, s_profile_names[i], n);
        pos += n;
    }
    buf[pos] = '\0';
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, CLAW_NVS_KEY_PROFILE_LIST, buf);
        nvs_commit(h);
        nvs_close(h);
    }
}

void claw_nvs_set_active_profile(const char *name)
{
    strncpy(s_active_profile, name, CLAW_PROFILE_NAME_MAX);
    s_active_profile[CLAW_PROFILE_NAME_MAX] = '\0';
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, CLAW_NVS_KEY_ACTIVE_PROFILE, s_active_profile);
        nvs_commit(h);
        nvs_close(h);
    }
}

void claw_nvs_migrate_wifi_profiles(void)
{
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    // Check if already migrated
    char buf[CLAW_PROFILE_NAME_MAX + 1] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, CLAW_NVS_KEY_ACTIVE_PROFILE, buf, &len) == ESP_OK) {
        nvs_close(h);
        return; // Already migrated
    }

    // Check for legacy ssid/pass
    char legacy_ssid[33] = {0};
    char legacy_pass[65] = {0};
    len = sizeof(legacy_ssid);
    bool has_legacy = (nvs_get_str(h, CLAW_NVS_KEY_SSID, legacy_ssid, &len) == ESP_OK);
    if (has_legacy) {
        len = sizeof(legacy_pass);
        nvs_get_str(h, CLAW_NVS_KEY_PASS, legacy_pass, &len);
        // Write to s_default / p_default
        nvs_set_str(h, "s_default", legacy_ssid);
        nvs_set_str(h, "p_default", legacy_pass);
        // Erase legacy keys
        nvs_erase_key(h, CLAW_NVS_KEY_SSID);
        nvs_erase_key(h, CLAW_NVS_KEY_PASS);
    }

    // Initialize prof_list and active_prof
    nvs_set_str(h, CLAW_NVS_KEY_PROFILE_LIST, CLAW_DEFAULT_PROFILE_NAME);
    nvs_set_str(h, CLAW_NVS_KEY_ACTIVE_PROFILE, CLAW_DEFAULT_PROFILE_NAME);
    nvs_commit(h);
    nvs_close(h);

    if (has_legacy) {
        ESP_LOGI(TAG, "Migrated legacy creds to profile '%s'",
                 CLAW_DEFAULT_PROFILE_NAME);
    }
}

void claw_nvs_profiles_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_profile_count = 0;
        strncpy(s_active_profile, CLAW_DEFAULT_PROFILE_NAME, CLAW_PROFILE_NAME_MAX);
        return;
    }
    // Load profile list
    char list_buf[CLAW_PROFILE_MAX_COUNT * (CLAW_PROFILE_NAME_MAX + 1) + 4] = {0};
    size_t len = sizeof(list_buf);
    if (nvs_get_str(h, CLAW_NVS_KEY_PROFILE_LIST, list_buf, &len) == ESP_OK) {
        profile_list_parse(list_buf);
    }
    // Ensure default always exists in list
    if (claw_nvs_profile_find(CLAW_DEFAULT_PROFILE_NAME) < 0) {
        claw_nvs_profile_add(CLAW_DEFAULT_PROFILE_NAME);
    }
    // Load active profile
    len = sizeof(s_active_profile);
    if (nvs_get_str(h, CLAW_NVS_KEY_ACTIVE_PROFILE, s_active_profile, &len) != ESP_OK) {
        strncpy(s_active_profile, CLAW_DEFAULT_PROFILE_NAME, CLAW_PROFILE_NAME_MAX);
    }
    nvs_close(h);
    // If active not in list, fall back to default
    if (claw_nvs_profile_find(s_active_profile) < 0) {
        strncpy(s_active_profile, CLAW_DEFAULT_PROFILE_NAME, CLAW_PROFILE_NAME_MAX);
    }
}

// ---------------------------------------------------------------------------
// WiFi credentials (per-profile)
// ---------------------------------------------------------------------------
bool claw_nvs_load_profile_creds(const char *name, char *ssid, char *pass)
{
    ssid[0] = '\0';
    pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    size_t len;
    build_profile_key('s', name, key);
    len = 33;
    esp_err_t err = nvs_get_str(h, key, ssid, &len);
    bool ok = (err == ESP_OK && strlen(ssid) > 0);
    build_profile_key('p', name, key);
    len = 65;
    if (nvs_get_str(h, key, pass, &len) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

void claw_nvs_save_profile_creds(const char *name, const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for save_profile_creds");
        return;
    }
    char key[16];
    build_profile_key('s', name, key);
    esp_err_t err = nvs_set_str(h, key, ssid);
    if (err != ESP_OK) ESP_LOGE(TAG, "write ssid err %d", err);
    build_profile_key('p', name, key);
    err = nvs_set_str(h, key, pass);
    if (err != ESP_OK) ESP_LOGE(TAG, "write pass err %d", err);
    nvs_commit(h);
    nvs_close(h);
}

void claw_nvs_erase_profile_creds(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    build_profile_key('s', name, key);
    nvs_erase_key(h, key);
    build_profile_key('p', name, key);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

bool claw_nvs_load_wifi_creds(char *ssid, char *pass)
{
    claw_nvs_profiles_load();
    return claw_nvs_load_profile_creds(s_active_profile, ssid, pass);
}

void claw_nvs_save_wifi_creds(const char *ssid, const char *pass)
{
    claw_nvs_save_profile_creds(s_active_profile, ssid, pass);
}

void claw_nvs_clear_wifi_creds(char *ssid, char *pass)
{
    bool was_default = (strcmp(s_active_profile, CLAW_DEFAULT_PROFILE_NAME) == 0);
    char cleared_name[CLAW_PROFILE_NAME_MAX + 1];
    strncpy(cleared_name, s_active_profile, CLAW_PROFILE_NAME_MAX + 1);

    claw_nvs_erase_profile_creds(s_active_profile);
    ssid[0] = '\0';
    pass[0] = '\0';

    if (!was_default) {
        claw_nvs_profile_remove(cleared_name);
        claw_nvs_profile_list_save();
        claw_nvs_set_active_profile(CLAW_DEFAULT_PROFILE_NAME);
        claw_nvs_load_profile_creds(CLAW_DEFAULT_PROFILE_NAME, ssid, pass);
    }
}

// ---------------------------------------------------------------------------
// MQTT broker
// ---------------------------------------------------------------------------
void claw_nvs_load_mqtt_broker(char *buf, int buf_len)
{
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = (size_t)buf_len;
        if (nvs_get_str(h, CLAW_NVS_KEY_MQTT_BROKER, buf, &len) != ESP_OK) {
            strncpy(buf, CLAW_MQTT_DEFAULT_BROKER, buf_len - 1);
            buf[buf_len - 1] = '\0';
        }
        nvs_close(h);
    } else {
        strncpy(buf, CLAW_MQTT_DEFAULT_BROKER, buf_len - 1);
        buf[buf_len - 1] = '\0';
    }
}

void claw_nvs_save_mqtt_broker(const char *uri)
{
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, CLAW_NVS_KEY_MQTT_BROKER, uri);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------------------
// Sound preference
// ---------------------------------------------------------------------------
bool claw_nvs_load_sound_pref(void)
{
    nvs_handle_t h;
    bool enabled = false;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, CLAW_NVS_KEY_SOUND, &val) == ESP_OK) {
            enabled = (val != 0);
        }
        nvs_close(h);
    }
    return enabled;
}

void claw_nvs_save_sound_pref(bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, CLAW_NVS_KEY_SOUND, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------------------
// Display rotation
// ---------------------------------------------------------------------------
uint8_t claw_nvs_load_rotation(void)
{
    nvs_handle_t h;
    uint8_t rotation = 0;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, CLAW_NVS_KEY_ROTATION, &val) == ESP_OK) {
            rotation = val & 3;
        }
        nvs_close(h);
    }
    return rotation;
}

void claw_nvs_save_rotation(uint8_t rotation)
{
    nvs_handle_t h;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, CLAW_NVS_KEY_ROTATION, rotation & 3);
        nvs_commit(h);
        nvs_close(h);
    }
}
