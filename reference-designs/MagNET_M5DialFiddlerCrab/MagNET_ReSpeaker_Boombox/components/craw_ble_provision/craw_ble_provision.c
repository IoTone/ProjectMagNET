/*
 * craw_ble_provision — NimBLE GATT provisioning service for MagNET hive nodes.
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_ble_provision.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_system.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_bt.h"

static const char *TAG = "craw_ble_prov";

#define DEFAULT_NAME_PREFIX "MagNET-biologic"
#define DEVICE_NAME_MAX     32
#define SSID_MAX            32
#define PASS_MAX            64
#define IP_MAX              20
#define ROLE_MAX            24

/* ---- Module state ---- */
static char                     s_device_name[DEVICE_NAME_MAX] = {0};
static char                     s_role[ROLE_MAX]               = {0};
static char                     s_ssid[SSID_MAX + 1]           = {0};
static char                     s_pass[PASS_MAX + 1]           = {0};
static char                     s_ip[IP_MAX]                   = "N/A";
static uint8_t                  s_status                       = CRAW_BLE_PROV_IDLE;
static craw_ble_provision_cb_t  s_cb                           = NULL;
static void                    *s_cb_ctx                       = NULL;

static uint16_t s_info_handle;
static uint16_t s_ssid_handle;
static uint16_t s_pass_handle;
static uint16_t s_commit_handle;
static uint16_t s_ip_handle;
static uint16_t s_status_handle;

/* When true, gap_event_handler does not auto-restart advertising on
 * disconnect. Set by craw_ble_provision_stop_advertising(); cleared by
 * craw_ble_provision_advertise(). Lets the app halt beacons once WiFi is
 * up (BLE beacons couple into audio amps on some boards — notably Atom
 * Echo — and cause periodic clicks). */
static bool s_adv_suppressed = false;

static void ble_start_advertising_internal(void);

/* ---- Service / characteristic UUIDs ----
 * Canonical UUIDs:
 *   svc     = 4d41474e-4554-0001-0000-000000000000  ("MAGNET" + 0001)
 *   chr N   = 4d41474e-4554-0001-0000-00000000000N  (N = 1..6)
 *
 * NimBLE BLE_UUID128_INIT() takes 16 bytes in little-endian order (i.e. the
 * canonical UUID reversed). The first argument is the low byte, which we
 * overload as the per-characteristic discriminator.
 */
#define MAGNET_UUID_FILL \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x01, 0x00, 0x54, 0x45, 0x4e, 0x47, 0x41, 0x4d

static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0x00, MAGNET_UUID_FILL);

static const ble_uuid128_t chr_info_uuid =
    BLE_UUID128_INIT(0x01, MAGNET_UUID_FILL);
static const ble_uuid128_t chr_ssid_uuid =
    BLE_UUID128_INIT(0x02, MAGNET_UUID_FILL);
static const ble_uuid128_t chr_pass_uuid =
    BLE_UUID128_INIT(0x03, MAGNET_UUID_FILL);
static const ble_uuid128_t chr_commit_uuid =
    BLE_UUID128_INIT(0x04, MAGNET_UUID_FILL);
static const ble_uuid128_t chr_ip_uuid =
    BLE_UUID128_INIT(0x05, MAGNET_UUID_FILL);
static const ble_uuid128_t chr_status_uuid =
    BLE_UUID128_INIT(0x06, MAGNET_UUID_FILL);

/* ---- device_info JSON builder ---- */
static void build_device_info(char *out, size_t out_len) {
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    const char *model = "unknown";
    switch (ci.model) {
        case CHIP_ESP32:   model = "ESP32";    break;
        case CHIP_ESP32S2: model = "ESP32-S2"; break;
        case CHIP_ESP32S3: model = "ESP32-S3"; break;
        case CHIP_ESP32C3: model = "ESP32-C3"; break;
        case CHIP_ESP32C6: model = "ESP32-C6"; break;
        case CHIP_ESP32H2: model = "ESP32-H2"; break;
        default: break;
    }
    snprintf(out, out_len,
        "{\"name\":\"%s\",\"role\":\"%s\",\"chip\":\"%s\",\"rev\":%d,"
        "\"fw\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"status\":%u}",
        s_device_name,
        s_role[0] ? s_role : "",
        model, ci.revision,
        CRAW_BLE_PROVISION_VERSION,
        s_ssid,
        s_ip,
        (unsigned)s_status);
}

/* ---- Characteristic access callbacks ---- */

static int chr_access_info(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char buf[256];
        build_device_info(buf, sizeof(buf));
        os_mbuf_append(ctxt->om, buf, strlen(buf));
    }
    return 0;
}

static int chr_access_ssid(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, s_ssid, strlen(s_ssid));
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > SSID_MAX) len = SSID_MAX;
        memset(s_ssid, 0, sizeof(s_ssid));
        ble_hs_mbuf_to_flat(ctxt->om, s_ssid, len, NULL);
        ESP_LOGI(TAG, "SSID written: '%s'", s_ssid);
        s_status = CRAW_BLE_PROV_CREDS_RECEIVED;
        if (s_cb) s_cb(CRAW_BLE_PROV_CREDS_RECEIVED, s_ssid, s_pass, s_cb_ctx);
    }
    return 0;
}

static int chr_access_pass(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    /* Reads return empty string (never echo stored password). */
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > PASS_MAX) len = PASS_MAX;
        memset(s_pass, 0, sizeof(s_pass));
        ble_hs_mbuf_to_flat(ctxt->om, s_pass, len, NULL);
        ESP_LOGI(TAG, "pass written (%d chars)", (int)strlen(s_pass));
        s_status = CRAW_BLE_PROV_CREDS_RECEIVED;
        if (s_cb) s_cb(CRAW_BLE_PROV_CREDS_RECEIVED, s_ssid, s_pass, s_cb_ctx);
    }
    return 0;
}

static int chr_access_commit(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t trigger = 0;
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= 1) ble_hs_mbuf_to_flat(ctxt->om, &trigger, 1, NULL);
        if (trigger) {
            ESP_LOGI(TAG, "commit triggered; ssid='%s'", s_ssid);
            s_status = CRAW_BLE_PROV_COMMIT_REQUESTED;
            if (s_cb) s_cb(CRAW_BLE_PROV_COMMIT_REQUESTED, s_ssid, s_pass, s_cb_ctx);
        }
    }
    return 0;
}

static int chr_access_ip(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, s_ip, strlen(s_ip));
    }
    return 0;
}

static int chr_access_status(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &s_status, sizeof(s_status));
    }
    return 0;
}

/* ---- GATT service definition ---- */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   /* device_info */
                .uuid      = &chr_info_uuid.u,
                .access_cb = chr_access_info,
                .flags     = BLE_GATT_CHR_F_READ,
                .val_handle = &s_info_handle,
            },
            {   /* wifi_ssid */
                .uuid      = &chr_ssid_uuid.u,
                .access_cb = chr_access_ssid,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_ssid_handle,
            },
            {   /* wifi_pass (write only) */
                .uuid      = &chr_pass_uuid.u,
                .access_cb = chr_access_pass,
                .flags     = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_pass_handle,
            },
            {   /* wifi_commit (write trigger) */
                .uuid      = &chr_commit_uuid.u,
                .access_cb = chr_access_commit,
                .flags     = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_commit_handle,
            },
            {   /* ip_address */
                .uuid      = &chr_ip_uuid.u,
                .access_cb = chr_access_ip,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ip_handle,
            },
            {   /* status */
                .uuid      = &chr_status_uuid.u,
                .access_cb = chr_access_status,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP event handler + advertising ---- */

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status=%d", event->connect.status);
        if (event->connect.status != 0 && !s_adv_suppressed) {
            ble_start_advertising_internal();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        if (!s_adv_suppressed) ble_start_advertising_internal();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_adv_suppressed) ble_start_advertising_internal();
        break;
    default:
        break;
    }
    return 0;
}

static void ble_start_advertising_internal(void) {
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields = {0};
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = (uint8_t *)s_device_name;
    fields.name_len         = strlen(s_device_name);
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, gap_event_handler, NULL);
    ESP_LOGI(TAG, "advertising as '%s'", s_device_name);
}

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void) {
    ble_start_advertising_internal();
}

/* ---- Derived device name from MAC ---- */

static void derive_device_name(const char *prefix) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_name, sizeof(s_device_name), "%s-%02x%02x",
             prefix ? prefix : DEFAULT_NAME_PREFIX, mac[4], mac[5]);
}

/* ---- Public API ---- */

void craw_ble_provision_init(const craw_ble_provision_config_t *cfg,
                             craw_ble_provision_cb_t cb,
                             void *cb_ctx) {
    const char *prefix = (cfg && cfg->name_prefix) ? cfg->name_prefix : DEFAULT_NAME_PREFIX;
    derive_device_name(prefix);

    if (cfg && cfg->role) {
        strncpy(s_role, cfg->role, sizeof(s_role) - 1);
    }
    s_cb     = cb;
    s_cb_ctx = cb_ctx;
    s_status = CRAW_BLE_PROV_IDLE;

    /* Log the DRAM budget so future allocation failures here are obvious.
     * BLE controller HCI buffers + NimBLE host need ~40 KB of internal RAM
     * combined; getting under that produces a 'hci inits failed' error
     * followed by a NULL-deref crash in ble_host_task. */
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "DRAM budget pre-NimBLE: free=%u largest=%u (need ~40 KB)",
             (unsigned)free_internal, (unsigned)largest_block);

    int rc = nimble_port_init();
    if (rc != 0) {
        /* Most common cause: insufficient internal RAM. Don't spawn the
         * host task — running nimble_port_run() against an uninitialized
         * event queue dereferences NULL. Caller can still proceed without
         * BLE; only provisioning will be unavailable. */
        ESP_LOGE(TAG, "nimble_port_init failed (rc=%d). BLE provisioning DISABLED. "
                      "free_internal=%u largest=%u",
                 rc, (unsigned)free_internal, (unsigned)largest_block);
        return;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_device_name_set(s_device_name);

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "initialized as '%s'", s_device_name);
}

void craw_ble_provision_set_status(craw_ble_prov_state_t state) {
    s_status = (uint8_t)state;
    ble_gatts_chr_updated(s_status_handle);
}

void craw_ble_provision_set_ip(const char *ip_str) {
    if (!ip_str) ip_str = "N/A";
    strncpy(s_ip, ip_str, sizeof(s_ip) - 1);
    s_ip[sizeof(s_ip) - 1] = '\0';
    ble_gatts_chr_updated(s_ip_handle);
}

void craw_ble_provision_advertise(void) {
    s_adv_suppressed = false;
    ble_start_advertising_internal();
}

void craw_ble_provision_stop_advertising(void) {
    s_adv_suppressed = true;
    ble_gap_adv_stop();
    ESP_LOGI(TAG, "advertising stopped");
}

void craw_ble_provision_deinit(void) {
    /* Tear down in reverse of init. Calling this while NimBLE has in-flight
     * callbacks is risky — callers should only invoke from a task that is
     * not the NimBLE host task. */
    s_adv_suppressed = true;
    ble_gap_adv_stop();

    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGW(TAG, "nimble_port_stop rc=%d", rc);
    }
    /* esp_bt_mem_release frees ~50 KB of controller RAM back to the heap on
     * classic ESP32. Must be called after the controller is disabled. */
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_mem_release(ESP_BT_MODE_BTDM);
    ESP_LOGI(TAG, "BLE stack fully deinitialized");
}

const char *craw_ble_provision_device_name(void) {
    return s_device_name;
}
