// ear_ble — the Ear's BLE peripheral. See ear_ble.h.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ear_ble.h"
#include "ear_atvv.h"

static const char *TAG = "ear_ble";

#ifndef EAR_DEV_JUSTWORKS
#define EAR_DEV_JUSTWORKS 0
#endif

/* Not exported by a public header in IDF 5.3's NimBLE port. */
void ble_store_config_init(void);

#define APPEARANCE_GENERIC_REMOTE  0x0180

static ear_ble_config_t s_cfg;
static ear_ble_cb_t     s_cb;
static char             s_name[20] = "MagNET-Ear";
static char             s_addr[18] = "?";
static uint8_t          s_own_addr_type;
static volatile int     s_conns;
static volatile bool    s_adv;
static volatile bool    s_encrypted;
static uint8_t          s_batt = 100;
static uint16_t         s_batt_handle;
static volatile int64_t s_pair_until;        /* esp_timer us; 0 = window closed */
static volatile int     s_pairing_conn = -1; /* conn mid-pairing (passkey shown) */

static bool pairable(void) { return s_pair_until > esp_timer_get_time(); }

static volatile bool    s_fast_adv;          /* wake: find me quickly */

static void emit(ear_ble_evt_t e, uint32_t arg) { if (s_cb) s_cb(e, arg); }

static void log_conn_params(uint16_t conn, const char *why) {
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return;
    ESP_LOGI(TAG, "%s: interval %u.%02u ms latency %u timeout %u ms", why,
             d.conn_itvl * 125 / 100, (d.conn_itvl * 125) % 100, d.conn_latency,
             d.supervision_timeout * 10);
    ear_atvv_on_conn_params(conn, d.conn_itvl);
}
static void advertise(void);

/* ------------------------------------------------------------ GATT */

static int chr_str(struct os_mbuf *om, const char *s) {
    return os_mbuf_append(om, s, strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int dis_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    switch (ble_uuid_u16(ctxt->chr->uuid)) {
    case 0x2A29: return chr_str(ctxt->om, "IoTone MagNET");
    case 0x2A24: return chr_str(ctxt->om, s_cfg.model);
    case 0x2A25: return chr_str(ctxt->om, s_cfg.serial);
    case 0x2A26: return chr_str(ctxt->om, s_cfg.fw_rev);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int bas_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, &s_batt, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_dis = {
        /* Device Information */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A24), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A25), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A26), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
};
static const struct ble_gatt_svc_def s_bas = {
        /* Battery */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = bas_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_batt_handle },
            { 0 },
        },
};
/* Built at init: C can't copy the extern ATVV def into a static initializer. */
static struct ble_gatt_svc_def s_svcs[4];

/* ------------------------------------------------------------ GAP */

static int gap_event(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conns++;
            s_adv = false;
            ESP_LOGI(TAG, "connected (handle %d), %d conn(s)", ev->connect.conn_handle, s_conns);
            ear_atvv_on_connect(ev->connect.conn_handle);
            log_conn_params(ev->connect.conn_handle, "connect");
            /* Data Length Extension: one 131-byte audio notification per
             * radio packet instead of 5 x 27-byte fragments. Without it the
             * link sometimes managed ~1 notification per 30 ms event (35 of
             * the 62.5 frames/s needed) and audio queued up (2026-10-07). */
            int dl = ble_gap_set_data_len(ev->connect.conn_handle, 251, 2120);
            if (dl) ESP_LOGW(TAG, "set_data_len rc=%d", dl);
            emit(EAR_BLE_EVT_CONNECTED, s_conns);
        }
        if (ev->connect.status != 0 || s_conns < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) advertise();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (s_conns > 0) s_conns--;
        ear_atvv_on_disconnect(ev->disconnect.conn.conn_handle);
        if (ev->disconnect.conn.conn_handle == s_pairing_conn) s_pairing_conn = -1;
        s_encrypted = false;
        ESP_LOGI(TAG, "disconnected reason=0x%x, %d conn(s)", ev->disconnect.reason, s_conns);
        emit(EAR_BLE_EVT_DISCONNECTED, s_conns);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv = false;
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        bool ok = ev->enc_change.status == 0 &&
                  ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0 &&
                  d.sec_state.encrypted && d.sec_state.bonded &&
                  (d.sec_state.authenticated || EAR_DEV_JUSTWORKS);
        ear_atvv_on_enc(ev->enc_change.conn_handle);
        s_encrypted = ok;
        if (ev->enc_change.conn_handle == s_pairing_conn) {
            s_pairing_conn = -1;
            if (ok) s_pair_until = 0;              /* bonded: close the window */
        }
#if EAR_DEV_JUSTWORKS
        if (ok) s_pair_until = 0;                  /* no passkey step to track */
#endif
        ESP_LOGI(TAG, "encryption change status=%d bonded=%d", ev->enc_change.status, ok);
        emit(EAR_BLE_EVT_PAIRED, ok ? 1 : 0);
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (!pairable()) {
            ESP_LOGW(TAG, "pairing refused: window closed (press B)");
            ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            emit(EAR_BLE_EVT_PAIR_REFUSED, 0);
            return 0;
        }
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            s_pairing_conn = ev->passkey.conn_handle;
            struct ble_sm_io pk = { .action = BLE_SM_IOACT_DISP,
                                    .passkey = esp_random() % 1000000 };
            ESP_LOGI(TAG, "passkey %06lu", (unsigned long)pk.passkey);
            emit(EAR_BLE_EVT_PASSKEY, pk.passkey);
            ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Peer lost its bond but we kept ours: drop ours, let it re-pair —
         * only inside the pairing window, else keep the old bond. */
        struct ble_gap_conn_desc d;
        if (!pairable()) {
            emit(EAR_BLE_EVT_PAIR_REFUSED, 0);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
            ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ev->conn_update.status == 0) log_conn_params(ev->conn_update.conn_handle, "conn update");
        else ESP_LOGW(TAG, "conn update failed status=%d", ev->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(TAG, "data length: tx %u B / %u us, rx %u B / %u us",
                 ev->data_len_chg.max_tx_octets, ev->data_len_chg.max_tx_time,
                 ev->data_len_chg.max_rx_octets, ev->data_len_chg.max_rx_time);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        ear_atvv_on_mtu(ev->mtu.conn_handle, ev->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%d notify=%d reason=%d", ev->subscribe.attr_handle,
                 ev->subscribe.cur_notify, ev->subscribe.reason);
        ear_atvv_on_subscribe(ev->subscribe.conn_handle, ev->subscribe.attr_handle,
                              ev->subscribe.cur_notify);
        return 0;
    }
    return 0;
}

static void advertise(void) {
    if (s_adv || ble_gap_adv_active()) return;
    if (s_conns >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) return;
    /* Unpaired and not in pairing mode: stay invisible, like a real remote. */
    if (!pairable() && ear_ble_bond_count() == 0) return;

    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    f.appearance = APPEARANCE_GENERIC_REMOTE;
    f.appearance_is_present = 1;
    static const ble_uuid16_t bas = BLE_UUID16_INIT(0x180F);
    f.uuids16 = &bas;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 0;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    /* ATVV 128-bit UUID doesn't fit beside the name: scan response. */
    struct ble_hs_adv_fields sr = {0};
    sr.uuids128 = (ble_uuid128_t *)ear_atvv_svc.uuid;
    sr.num_uuids128 = 1;
    sr.uuids128_is_complete = 0;
    rc = ble_gap_adv_rsp_set_fields(&sr);
    if (rc) { ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 100–150 ms: findable quickly, cheap on the 120 mAh cell. 20–30 ms
     * while a talk press waits for its host (Apple's fast-advertising rate). */
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(s_fast_adv ? 20 : 100);
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(s_fast_adv ? 30 : 150);
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc == 0) s_adv = true;
    else ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uint8_t a[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, a, NULL);
    snprintf(s_addr, sizeof(s_addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             a[5], a[4], a[3], a[2], a[1], a[0]);
    ESP_LOGI(TAG, "sync: %s addr %s (type %d)", s_name, s_addr, s_own_addr_type);
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset, reason=%d", reason); }

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------ API */

int ear_ble_init(const ear_ble_config_t *cfg, ear_ble_cb_t cb) {
    s_cfg = *cfg;
    s_cb  = cb;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "MagNET-Ear-%02x%02x", mac[4], mac[5]);

    int rc = nimble_port_init();
    if (rc != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init rc=%d", rc); return rc; }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
#if EAR_DEV_JUSTWORKS
    /* BENCH ONLY (-DEAR_DEV_JUSTWORKS=1 on the command line, never in config):
     * Just Works pairing — encrypted + bonded but NOT MITM-protected. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_mitm = 0;
    ESP_LOGW(TAG, "DEV BUILD: Just Works pairing, no MITM protection");
#else
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_mitm = 1;
#endif
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    s_svcs[0] = s_dis;
    s_svcs[1] = s_bas;
    s_svcs[2] = ear_atvv_svc;
    memset(&s_svcs[3], 0, sizeof(s_svcs[3]));
    rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc) { ESP_LOGE(TAG, "gatts add rc=%d", rc); return rc; }
    ble_svc_gap_device_name_set(s_name);
    ble_svc_gap_device_appearance_set(APPEARANCE_GENERIC_REMOTE);

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return 0;
}

const char *ear_ble_name(void)  { return s_name; }
const char *ear_ble_addr(void)  { return s_addr; }
int  ear_ble_conn_count(void)   { return s_conns; }
bool ear_ble_advertising(void)  { return s_adv; }
bool ear_ble_encrypted(void)    { return s_encrypted; }

void ear_ble_battery_set(uint8_t pct) {
    if (pct > 100) pct = 100;
    if (pct == s_batt) return;
    s_batt = pct;
    if (s_batt_handle) ble_gatts_chr_updated(s_batt_handle);
}

int ear_ble_bond_count(void) {
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
    return n;
}

void ear_ble_pairing(bool open, int secs) {
    if (open) {
        s_pair_until = esp_timer_get_time() + (int64_t)secs * 1000000;
        ESP_LOGI(TAG, "pairing window open %d s", secs);
        advertise();
        return;
    }
    s_pair_until = 0;
    int c = s_pairing_conn;
    if (c >= 0) ble_gap_terminate((uint16_t)c, BLE_ERR_REM_USER_CONN_TERM);
    if (ear_ble_bond_count() == 0 && ble_gap_adv_active()) {
        ble_gap_adv_stop();
        s_adv = false;
    }
    ESP_LOGI(TAG, "pairing window closed");
}

void ear_ble_wake(bool on) {
    if (s_fast_adv == on) return;
    s_fast_adv = on;
    if (ble_gap_adv_active()) { ble_gap_adv_stop(); s_adv = false; }
    advertise();
}

int ear_ble_pairing_left_s(void) {
    int64_t left = s_pair_until - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

void ear_ble_unpair_all(void) {
    ble_store_clear();
}
