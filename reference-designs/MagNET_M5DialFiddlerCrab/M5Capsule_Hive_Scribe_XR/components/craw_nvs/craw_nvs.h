#ifndef CRAW_NVS_H
#define CRAW_NVS_H
#define CRAW_NVS_VERSION "0.1.0"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define CRAW_NVS_NAMESPACE          "craw_config"
#define CRAW_NVS_KEY_SSID           "ssid"          // Legacy — migrated to s_default
#define CRAW_NVS_KEY_PASS           "pass"          // Legacy — migrated to p_default
#define CRAW_NVS_KEY_MQTT_BROKER    "mqtt_url"
#define CRAW_NVS_KEY_SOUND          "sound"
#define CRAW_NVS_KEY_PROFILE_LIST   "prof_list"
#define CRAW_NVS_KEY_ACTIVE_PROFILE "active_prof"
#define CRAW_NVS_KEY_ROTATION       "display_rot"

#define CRAW_PROFILE_NAME_MAX       12
#define CRAW_PROFILE_MAX_COUNT      5
#define CRAW_DEFAULT_PROFILE_NAME   "default"
#define CRAW_MQTT_DEFAULT_BROKER    "mqtt://broker.hivemq.com:1883"

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

// Initialize NVS flash. Call once at boot before any other craw_nvs calls.
void craw_nvs_init_flash(void);

// ---------------------------------------------------------------------------
// Profile management
// ---------------------------------------------------------------------------

// Get current active profile name (read-only pointer to internal buffer).
const char *craw_nvs_active_profile(void);

// Get profile list. Sets *count to the number of profiles.
// Returns pointer to internal array — do not free.
const char (*craw_nvs_profile_list(int *count))[CRAW_PROFILE_NAME_MAX + 1];

// Get profile count.
int craw_nvs_profile_count(void);

// Validate profile name: 1..PROFILE_NAME_MAX chars, [A-Za-z0-9_-].
bool craw_nvs_profile_name_valid(const char *name);

// Find profile index by name. Returns -1 if not found.
int craw_nvs_profile_find(const char *name);

// Add a profile to the in-memory list. Returns false if full or duplicate.
bool craw_nvs_profile_add(const char *name);

// Remove a profile from the in-memory list.
void craw_nvs_profile_remove(const char *name);

// Save in-memory profile list to NVS.
void craw_nvs_profile_list_save(void);

// Set active profile (updates NVS and in-memory state).
void craw_nvs_set_active_profile(const char *name);

// One-time migration from legacy ssid/pass to profile-based schema.
void craw_nvs_migrate_wifi_profiles(void);

// Load profile list and active profile from NVS into memory.
void craw_nvs_profiles_load(void);

// ---------------------------------------------------------------------------
// WiFi credentials (per-profile)
// ---------------------------------------------------------------------------

// Load credentials for a named profile into ssid/pass buffers.
// ssid must be >= 33 bytes, pass >= 65 bytes.
// Returns true if SSID was found (non-empty).
bool craw_nvs_load_profile_creds(const char *name, char *ssid, char *pass);

// Save ssid/pass to a named profile in NVS.
void craw_nvs_save_profile_creds(const char *name, const char *ssid, const char *pass);

// Erase credentials for a named profile from NVS.
void craw_nvs_erase_profile_creds(const char *name);

// Load credentials for the currently active profile.
// ssid must be >= 33 bytes, pass >= 65 bytes.
// Also reloads profile list from NVS first.
// Returns true if SSID is configured.
bool craw_nvs_load_wifi_creds(char *ssid, char *pass);

// Save ssid/pass to the active profile.
void craw_nvs_save_wifi_creds(const char *ssid, const char *pass);

// Clear credentials for the active profile. If non-default, also removes
// the profile from the list and switches active back to default.
// ssid/pass buffers are cleared. ssid >= 33 bytes, pass >= 65 bytes.
void craw_nvs_clear_wifi_creds(char *ssid, char *pass);

// ---------------------------------------------------------------------------
// MQTT broker
// ---------------------------------------------------------------------------

// Load MQTT broker URI into buf (buf_len should be >= 128).
void craw_nvs_load_mqtt_broker(char *buf, int buf_len);

// Save MQTT broker URI from buf.
void craw_nvs_save_mqtt_broker(const char *uri);

// ---------------------------------------------------------------------------
// Sound preference
// ---------------------------------------------------------------------------

// Load sound enabled preference. Returns the saved value (false if unset).
bool craw_nvs_load_sound_pref(void);

// Save sound enabled preference.
void craw_nvs_save_sound_pref(bool enabled);

// ---------------------------------------------------------------------------
// Display rotation
// ---------------------------------------------------------------------------

// Load rotation value (0..3). Returns 0 if unset.
uint8_t craw_nvs_load_rotation(void);

// Save rotation value (0..3).
void craw_nvs_save_rotation(uint8_t rotation);

#ifdef __cplusplus
}
#endif
#endif
