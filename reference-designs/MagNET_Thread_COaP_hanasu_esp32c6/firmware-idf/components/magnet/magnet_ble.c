/*
 * magnet_ble.c — BLE-GATT HCP binding (§11.2.1), PROVISIONING-ONLY.
 *
 * Decision (§12.9 Q3, settled 2026-07-31): BLE runs only until the node has a
 * channel, then the whole controller is torn down and the C6's 2.4 GHz front
 * end belongs to 802.15.4 alone. Rationale: no continuous BLE/Thread
 * coexistence risk, ~40-60 KB of NimBLE RAM returned to the heap for the
 * lifetime of the node, and it matches the fleet lesson that BLE sharing a
 * radio/rail with another subsystem causes trouble (see the Atom Echo
 * BLE-audio coupling note).
 *
 * What a phone can therefore do: scan, bond, read CAPS, write CHANNEL SET /
 * NAME, and watch !STATE — i.e. onboard a node with no cable. What it cannot
 * do: stay attached as a live chat client. That needs the resident-BLE build,
 * which is deliberately not this one.
 *
 * The GATT service is a byte pipe: exactly the same HCP line grammar as
 * USB-CDC, so the host SDK's framing layer is unchanged (that is the whole
 * point of §11.2). Writes are buffered until '\n'; notifications chunk at
 * (MTU-3) and the client reassembles on '\n'.
 *
 * Build with MN_ENABLE_BLE=1 (env esp32c6_ble). Default builds compile this
 * out entirely so the stock firmware carries zero BLE cost.
 */
#include "magnet.h"

#if MN_ENABLE_BLE

#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define TAG "mn_ble"

/* §11.2.1 provisional UUIDs: 6d61676e-2d68-6370-0001-0000000000NN */
/* Displayed form (§11.2.1): 6d61676e-2d68-6370-0001-0000000000NN
 * = ASCII "magn" "-h" "cp". BLE_UUID128_INIT wants the bytes REVERSED. */
/* Displayed form (§11.2.1): 6d61676e-2d68-6370-0001-0000000000NN — ASCII
 * "magn" "-h" "cp". BLE_UUID128_INIT takes EXACTLY 16 bytes in REVERSE order;
 * passing more silently truncates and leaves the wrong UUID on the air. */
static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x01, 0x00, 0x70, 0x63, 0x68, 0x2d, 0x6e, 0x67,
                     0x61, 0x6d);
static const ble_uuid128_t CHR_CMD_UUID =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x01, 0x00, 0x70, 0x63, 0x68, 0x2d, 0x6e, 0x67,
                     0x61, 0x6d);
static const ble_uuid128_t CHR_EVT_UUID =
    BLE_UUID128_INIT(0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x01, 0x00, 0x70, 0x63, 0x68, 0x2d, 0x6e, 0x67,
                     0x61, 0x6d);
/* HCP-AUTH (…0003): read-only, encryption-required. Its whole job is to give
 * the central a reason to pair.
 *
 * A central initiates pairing when an *operation* needs encryption — it will
 * happily ignore a peripheral's Security Request otherwise. Once bonding moved
 * to per-verb enforcement, nothing in the GATT table required encryption any
 * more, so no phone ever paired and every privileged verb answered
 * E_NOT_BONDED forever. Reading this attribute is the trigger, and unlike
 * Android's createBond() it works on iOS too. */
static const ble_uuid128_t CHR_AUTH_UUID =
    BLE_UUID128_INIT(0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x01, 0x00, 0x70, 0x63, 0x68, 0x2d, 0x6e, 0x67,
                     0x61, 0x6d);

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_evt_handle;
static bool     s_running = false;
static bool     s_encrypted = false;   /* link is bonded/encrypted right now */
static char     s_rx[512];
static size_t   s_rx_len = 0;

/* Command lines are dispatched on a worker task, never in the ATT callback.
 * The callback must return before NimBLE will acknowledge the write, and a
 * verb can take a long time — CAPS alone pushes several notification chunks.
 * Doing that work inline made the host's write time out even though the node
 * had answered correctly, and any delay in the callback stalls the whole
 * host task. Same discipline as the OpenThread event pump (§12.4). */
#define MN_BLE_LINE_MAX 256
static QueueHandle_t s_rx_q = NULL;

static void ble_post_line(const char *line) {
    if (!s_rx_q) return;
    char item[MN_BLE_LINE_MAX];
    strlcpy(item, line, sizeof(item));
    xQueueSend(s_rx_q, item, 0);       /* never block the ATT callback */
}

static void ble_worker(void *arg) {
    (void)arg;
    char item[MN_BLE_LINE_MAX];
    for (;;) {
        if (xQueueReceive(s_rx_q, item, portMAX_DELAY) == pdTRUE) {
            mn_link_feed_line(item);   /* same dispatcher as USB-CDC */
        }
    }
}

/* ---- line assembly ----
 * Same grammar as the serial binding, but with one BLE-specific rule: an ATT
 * write is itself a message boundary, so a write that does not end in a
 * newline is still dispatched when it completes. Real clients (and every
 * scanner app) send "STATUS", not "STATUS\n" — requiring the newline made the
 * binding unusable from a phone. Continuation is still supported: a write that
 * exactly fills the negotiated payload is assumed to be mid-line and is held
 * until the rest (or a newline) arrives. */
static void ble_feed(const uint8_t *data, size_t len, bool write_complete) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_rx_len) {
                s_rx[s_rx_len] = '\0';
                ble_post_line(s_rx);           /* dispatched off-callback */
                s_rx_len = 0;
            }
            write_complete = false;            /* newline already flushed it */
        } else if (s_rx_len < sizeof(s_rx) - 1) {
            s_rx[s_rx_len++] = c;
        }
    }
    if (write_complete && s_rx_len) {
        s_rx[s_rx_len] = '\0';
        ble_post_line(s_rx);
        s_rx_len = 0;
    }
}

/* ---- outbound: chunk to (MTU-3), client reassembles at '\n' ---- */
void mn_ble_notify(const char *line) {
    if (!s_running || s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    uint16_t mtu = ble_att_mtu(s_conn);
    size_t chunk = (mtu > 23 ? mtu : 23) - 3;
    size_t len = strlen(line);
    char buf[256];

    for (size_t off = 0; off <= len; off += chunk) {
        size_t n = len - off;
        bool last = false;
        if (n > chunk) n = chunk; else last = true;
        size_t m = n;
        memcpy(buf, line + off, m);
        if (last && m < sizeof(buf) - 1) buf[m++] = '\n';   /* frame terminator */

        /* Fire-and-forget loses chunks. NimBLE's mbuf pool is small and a
         * burst (a long CAPS line is several chunks) exhausts it; the notify
         * then fails and the host waits forever for a line that can never be
         * completed — one dropped chunk means no terminating newline. Retry
         * with a short yield, which also lets the stack drain. */
        int rc = BLE_HS_ENOMEM;
        for (int attempt = 0; attempt < 12 && rc != 0; attempt++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, m);
            if (om) {
                rc = ble_gattc_notify_custom(s_conn, s_evt_handle, om);
            }
            if (rc != 0) vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (rc != 0) return;               /* link is gone or hopelessly full */
        if (last) break;
    }
}

static int chr_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* reaching here at all means the link is encrypted */
        static const char ok[] = "bonded";
        return os_mbuf_append(ctxt->om, ok, sizeof(ok) - 1) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t tmp[256];
        if (len > sizeof(tmp)) len = sizeof(tmp);
        ble_hs_mbuf_to_flat(ctxt->om, tmp, len, NULL);
        /* a short write ends the message; a full-MTU write may be a fragment */
        uint16_t mtu = (conn != BLE_HS_CONN_HANDLE_NONE) ? ble_att_mtu(conn) : 23;
        bool complete = (len < (mtu > 3 ? mtu - 3 : 20));
        ble_feed(tmp, len, complete);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   /* HCP-CMD: host → node */
                .uuid = &CHR_CMD_UUID.u,
                .access_cb = chr_access,
                /* §11.2.1 requires bonding for config-changing writes, but
                 * every verb shares this characteristic — gating it at the ATT
                 * layer would also block read-only commands like STATUS, and
                 * would surface as an opaque ATT error the host can't act on.
                 * The bonding requirement is enforced per-verb instead, which
                 * is what the E_NOT_BONDED error code exists for (§11.3.4). */
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   /* HCP-EVT: node → host */
                .uuid = &CHR_EVT_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
                .val_handle = &s_evt_handle,
            },
            {   /* HCP-AUTH: read it to force pairing (see UUID comment) */
                .uuid = &CHR_AUTH_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            { 0 }
        },
    },
    { 0 }
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_encrypted = false;
            mn_emit_event("!STATE PROVISIONING ble-connected");
            /* The config characteristics are WRITE_ENC (§11.2.1: privileged
             * writes require a bonded link). A central has no way to know it
             * must pair until a write is rejected, and most stacks surface
             * that as a hard error rather than pairing and retrying. So the
             * peripheral asks for security immediately — by the time the host
             * sends its first command the link is already encrypted. */
            int rc = ble_gap_security_initiate(s_conn);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                mn_emit_event("# ble: security_initiate rc=%d", rc);
            }
        } else {
            advertise();
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        s_encrypted = (event->enc_change.status == 0) &&
                      ble_gap_conn_find(event->enc_change.conn_handle, &d) == 0 &&
                      d.sec_state.encrypted;
        /* status is the SMP reason — 0 = paired. Non-zero values decode via
         * NimBLE's BLE_HS_SM_US_ERR()/BLE_HS_SM_PEER_ERR() ranges and are the
         * only way to tell "phone refused" from "no keys" from "stale bond". */
        mn_emit_event("# ble: link %s (enc_change status=%d)",
                      s_encrypted ? "encrypted" : "NOT encrypted",
                      event->enc_change.status);
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The host forgot the bond (app deleted, device reset). Drop our copy
         * and let the fresh pairing proceed, otherwise the node becomes
         * permanently unpairable to that host. */
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_encrypted = false;
        if (s_running) advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_gap_adv_params adv = { 0 };
    struct ble_hs_adv_fields fields = { 0 };
    const char *name = ble_svc_gap_device_name();

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv, gap_event, NULL);
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    advertise();
    mn_emit_event("# ble: advertising for provisioning (torn down once a channel is set)");
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();                 /* returns when the port is stopped */
    nimble_port_freertos_deinit();
}

int mn_ble_start(void) {
    if (s_running) return 0;
    char name[24];
    const uint8_t *id = mn_device_id();
    snprintf(name, sizeof(name), "MagNET-%02x%02x", id[2], id[3]);

    if (nimble_port_init() != ESP_OK) return -1;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_bonding = 1;                 /* LE Secure Connections bond  */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;  /* screenless: Just Works */
    ble_hs_cfg.sm_sc = 1;
    /* Distribute identity keys as well as encryption keys: a bond that keeps
     * only an LTK cannot be re-established once either side changes address. */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* NB: the NVS-backed key store (ble_store_config_init) is wired up by
     * NimBLE's own sysinit when CONFIG_BT_NIMBLE_NVS_PERSIST=y — there is no
     * public header for it and calling it here does not compile. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_svcs) != 0 || ble_gatts_add_svcs(s_svcs) != 0) return -2;
    ble_svc_gap_device_name_set(name);

    if (!s_rx_q) {
        s_rx_q = xQueueCreate(6, MN_BLE_LINE_MAX);
        xTaskCreate(ble_worker, "mn_ble_rx", 6144, NULL, 5, NULL);
    }

    s_running = true;
    nimble_port_freertos_init(host_task);
    return 0;
}

/* Tear-down is the point of this binding: reclaim the RAM and the radio. */
int mn_ble_stop(void) {
    if (!s_running) return 0;
    s_running = false;
    ble_gap_adv_stop();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    int rc = nimble_port_stop();
    if (rc == 0) nimble_port_deinit();     /* frees the controller + host RAM */
    mn_emit_event("# ble: torn down (channel provisioned); radio is Thread-only now");
    return rc;
}

bool mn_ble_running(void) { return s_running; }

bool mn_ble_link_secure(void) { return s_encrypted; }

#else  /* MN_ENABLE_BLE == 0 */

int  mn_ble_start(void)  { return -1; }
bool mn_ble_link_secure(void) { return false; }
int  mn_ble_stop(void)   { return 0; }
bool mn_ble_running(void) { return false; }
void mn_ble_notify(const char *line) { (void)line; }

#endif
