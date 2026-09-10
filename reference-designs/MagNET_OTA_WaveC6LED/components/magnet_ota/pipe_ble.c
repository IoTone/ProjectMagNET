/*
 * pipe_ble — the BLE GATT pipe for transport_relay (plan R3).
 *
 * Extends the pattern proven by craw_ble_provision (NimBLE, one primary
 * service, flat characteristics) rather than importing it: that service is
 * WiFi provisioning with its own state machine, and this device provisions
 * over the console. What is borrowed is the shape and the UUID scheme.
 *
 *   service  4d41474e-4554-0002-0000-000000000000   "MAGNET" + 0002
 *   tx       ...0001   NOTIFY   device → proxy   (REQ frames)
 *   rx       ...0002   WRITE    proxy  → device  (RESP / ERR frames)
 *   info     ...0003   READ     JSON: name, fw, mtu, has_token, has_server
 *
 * "Attached" means a central is connected AND subscribed to tx. A phone that
 * merely connects (or a scanner app poking around) is not a proxy, and the
 * supervisor must not start pouring frames at it.
 *
 * Advertises as ROBOTARME-<mac4>: the phone filters by the service UUID (in
 * the scan response, since the 31-byte advertisement is full with flags +
 * name) and shows the name. The MAC suffix lets an operator with two boards
 * on the bench pick the right one.
 *
 * Coexistence: the C6 has one radio for WiFi and BLE. IDF's coex handles it;
 * the observable cost is BLE throughput while WiFi is busy, which the relay
 * tolerates by design (chunked, timeout-based). A device using the relay is
 * usually one WITHOUT WiFi anyway — that is the point of it.
 */
#include "sdkconfig.h"
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include <string.h>
#include <stdio.h>
#include "magnet_relay.h"
#include "relay_frame.h"
#include "magnet_cfg.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "pipe_ble";

#define NAME_PREFIX "ROBOTARME"
#define FW_TAG      "wavec6led-r3"

/* NimBLE wants the 128-bit UUID as 16 little-endian bytes: canonical
 * 4d41474e-4554-0002-0000-0000000000NN reversed, NN first. */
#define RELAY_UUID_FILL \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x02, 0x00, 0x54, 0x45, 0x4e, 0x47, 0x41, 0x4d
static const ble_uuid128_t svc_uuid  = BLE_UUID128_INIT(0x00, RELAY_UUID_FILL);
static const ble_uuid128_t tx_uuid   = BLE_UUID128_INIT(0x01, RELAY_UUID_FILL);
static const ble_uuid128_t rx_uuid   = BLE_UUID128_INIT(0x02, RELAY_UUID_FILL);
static const ble_uuid128_t info_uuid = BLE_UUID128_INIT(0x03, RELAY_UUID_FILL);

static char     s_name[24];
static uint16_t s_tx_handle, s_rx_handle, s_info_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool     s_subscribed;
static uint16_t s_mtu = 23;
static bool     s_up;

static void advertise(void);

/* ---- characteristics --------------------------------------------------- */

static int access_info(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return 0;
    char tok[CFG_MAX], url[CFG_MAX], buf[160];
    bool has_tok = cfg_get(CFG_DEV_TOKEN, tok, sizeof tok);
    bool has_url = cfg_get(CFG_SERVER_URL, url, sizeof url);
    snprintf(buf, sizeof buf,
             "{\"name\":\"%s\",\"fw\":\"%s\",\"proto\":%d,\"mtu\":%u,"
             "\"has_token\":%s,\"has_server\":%s}",
             s_name, FW_TAG, RELAY_FRAME_VERSION, (unsigned)s_mtu,
             has_tok ? "true" : "false", has_url ? "true" : "false");
    os_mbuf_append(ctxt->om, buf, strlen(buf));
    return 0;
}

static int access_rx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    /* One GATT write is one frame. A long write is reassembled by NimBLE
     * before we see it, so the frame is whole here. */
    static uint8_t buf[520];
    if (len > sizeof buf) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t out = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &out);
    relay_rx(buf, out);
    return 0;
}

static int access_tx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0;   /* notify-only; a read returns nothing */
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &tx_uuid.u,   .access_cb = access_tx,   .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_tx_handle },
            { .uuid = &rx_uuid.u,   .access_cb = access_rx,   .flags = BLE_GATT_CHR_F_WRITE,  .val_handle = &s_rx_handle },
            { .uuid = &info_uuid.u, .access_cb = access_info, .flags = BLE_GATT_CHR_F_READ,   .val_handle = &s_info_handle },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP ----------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_subscribed = false;
            s_mtu = ble_att_mtu(s_conn);
            ESP_LOGI(TAG, "central connected (mtu %u)", (unsigned)s_mtu);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason %d)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        ESP_LOGI(TAG, "mtu now %u", (unsigned)s_mtu);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = ev->subscribe.cur_notify;
            ESP_LOGI(TAG, "proxy %s", s_subscribed ? "ATTACHED" : "detached");
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (uint8_t *)s_name;
    adv.name_len = strlen(s_name);
    adv.name_is_complete = 1;
    ble_gap_adv_set_fields(&adv);
    /* Service UUID in the scan response: the phone filters on it. */
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv start rc=%d", rc);
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    advertise();
    ESP_LOGI(TAG, "advertising as %s", s_name);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- relay_pipe_t -------------------------------------------------------- */

static bool   ble_attached(void)  { return s_up && s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed; }
static size_t ble_max_frame(void) { return s_mtu > 3 ? (size_t)(s_mtu - 3) : 20; }

static int ble_write(const uint8_t *frame, size_t len) {
    if (!ble_attached()) return -1;
    /* The host's mbuf pool is finite and a burst of notifications drains it
     * faster than the controller sends; ENOMEM here is "not yet", not "no".
     * Back off and retry rather than dropping a frame, because the far end
     * has no way to ask for a resend and the whole request would be lost. */
    for (int attempt = 0; attempt < 50; ++attempt) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)len);
        if (!om) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
        if (rc == 0) return 0;
        if (rc != BLE_HS_ENOMEM) { ESP_LOGW(TAG, "notify rc=%d", rc); return -1; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "notify: mbuf pool exhausted for 500 ms");
    return -1;
}

static const relay_pipe_t s_pipe = {
    .name = "ble",
    .attached = ble_attached,
    .max_frame = ble_max_frame,
    .write = ble_write,
};

esp_err_t pipe_ble_init(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof s_name, NAME_PREFIX "-%02x%02x", mac[4], mac[5]);

    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init rc=%d (free %u) — BLE relay disabled", rc, (unsigned)before);
        return ESP_FAIL;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatt svc rc=%d", rc); return ESP_FAIL; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_device_name_set(s_name);
    /* Ask for the largest ATT MTU; the central decides. Android phones
     * generally grant 512, iOS 185. Either way the frame size follows. */
    ble_att_set_preferred_mtu(517);
    nimble_port_freertos_init(host_task);
    s_up = true;
    relay_register_pipe(&s_pipe);
    ESP_LOGI(TAG, "up as %s (heap %u -> %u)", s_name, (unsigned)before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

const char *pipe_ble_name(void) { return s_name; }
bool pipe_ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

#else  /* BLE compiled out: keep the symbols so main links either way */
#include "esp_err.h"
#include <stdbool.h>
esp_err_t   pipe_ble_init(void)      { return ESP_ERR_NOT_SUPPORTED; }
const char *pipe_ble_name(void)      { return "(no BLE in this build)"; }
bool        pipe_ble_connected(void) { return false; }
#endif
