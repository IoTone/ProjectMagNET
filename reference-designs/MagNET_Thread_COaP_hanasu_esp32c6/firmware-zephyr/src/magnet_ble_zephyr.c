/*
 * magnet_ble_zephyr.c — BLE-GATT HCP binding (§11.2.1) on the Zephyr BT host.
 *
 * Same contract as firmware-idf/components/magnet/magnet_ble.c (NimBLE):
 * mn_ble_start/stop/running/link_secure/notify, the same GATT service and
 * UUIDs, and every rule docs/BLE-PAIRING.md paid for:
 *   - HCP-AUTH (…0003) is read-encrypted: reading it is what makes a central
 *     pair (centrals ignore a peripheral's Security Request).
 *   - bonding is enforced per verb (E_NOT_BONDED in magnet_link.c), never at
 *     the ATT layer — HCP-CMD is a plain write.
 *   - never dispatch in the ATT write callback: lines go to a worker.
 *   - notifications retry on buffer exhaustion; one lost chunk = no newline.
 *   - message boundary: a write shorter or longer than (MTU-3) completes a
 *     line; exactly (MTU-3) may continue. Long (prepared) writes are
 *     reassembled by offset and flushed once the execute sequence ends.
 *   - Just Works + numeric-comparison auto-accept (screenless node), and a
 *     peer that lost its bond may pair again (SMP_ALLOW_UNAUTH_OVERWRITE).
 *
 * Unlike the C6 build (provisioning-only by default), the MG24's radio is
 * built for concurrent BLE + 802.15.4 (RAIL multiprotocol), so the resident
 * companion mode is the natural one here; MN_BLE_RESIDENT still selects it.
 *
 * Context discipline: BT host callbacks run on the BT RX thread, which must
 * never block — they use mn_post_note() (pump-emitted), never mn_emit_event.
 */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include <string.h>

#include "magnet.h"
#include "freertos/FreeRTOS.h"

/* 6d61676e-2d68-6370-0001-0000000000NN — ASCII "magn" "-h" "cp" */
#define MN_UUID(nn) BT_UUID_128_ENCODE(0x6d61676e, 0x2d68, 0x6370, 0x0001, (nn##ULL))
static const struct bt_uuid_128 SVC_UUID  = BT_UUID_INIT_128(MN_UUID(0x000000000000));
static const struct bt_uuid_128 CMD_UUID  = BT_UUID_INIT_128(MN_UUID(0x000000000001));
static const struct bt_uuid_128 EVT_UUID  = BT_UUID_INIT_128(MN_UUID(0x000000000002));
static const struct bt_uuid_128 AUTH_UUID = BT_UUID_INIT_128(MN_UUID(0x000000000003));

static struct bt_conn *s_conn;
static bool s_running;
static bool s_encrypted;

/* ---- inbound: assemble lines, hand them to a worker ---- */
#define MN_BLE_LINE_MAX 256
static char   s_rx[512];
static size_t s_rx_len;
static QueueHandle_t s_rx_q;

static void post_line(void) {
    if (!s_rx_len) return;
    s_rx[s_rx_len] = '\0';
    if (s_rx_q) {
        char item[MN_BLE_LINE_MAX];
        strncpy(item, s_rx, sizeof(item) - 1);
        item[sizeof(item) - 1] = '\0';
        xQueueSend(s_rx_q, item, 0);          /* never block the BT RX thread */
    }
    s_rx_len = 0;
}

static void feed(const uint8_t *data, size_t len, bool write_complete) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            post_line();
            write_complete = false;           /* newline already flushed it */
        } else if (s_rx_len < sizeof(s_rx) - 1) {
            s_rx[s_rx_len++] = c;
        }
    }
    if (write_complete) post_line();
}

static void ble_worker(void *arg) {
    ARG_UNUSED(arg);
    char item[MN_BLE_LINE_MAX];
    for (;;) {
        if (xQueueReceive(s_rx_q, item, portMAX_DELAY) == pdTRUE)
            mn_link_feed_line(item);          /* same dispatcher as the UART */
    }
}

/* A prepared (long) write arrives as a burst of EXECUTE callbacks with no
 * "last chunk" marker; flush the reassembled line shortly after the burst. */
static void long_write_flush(struct k_work *w) {
    ARG_UNUSED(w);
    post_line();
}
static K_WORK_DELAYABLE_DEFINE(s_long_flush, long_write_flush);

static ssize_t cmd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(attr);
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) return 0;      /* accept; data at execute */
    if (flags & BT_GATT_WRITE_FLAG_EXECUTE) {
        if (offset == 0) s_rx_len = 0;
        feed(buf, len, false);
        k_work_reschedule(&s_long_flush, K_MSEC(20));
        return len;
    }
    uint16_t payload = bt_gatt_get_mtu(conn) - 3;
    feed(buf, len, len != payload);
    return len;
}

static ssize_t auth_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset) {
    static const char ok[] = "bonded";       /* reaching here = link encrypted */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, ok, sizeof(ok) - 1);
}

static ssize_t evt_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, "", 0);
}

/* Subscription is NOT tracked here: for a bonded peer Zephyr restores the
 * stored CCC on reconnect and CoreBluetooth does not rewrite it, so a flag
 * cleared in connected() silently muted every reply after a reboot. The
 * stack is asked at send time instead (bt_gatt_is_subscribed). */
static void evt_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr); ARG_UNUSED(value);
}

BT_GATT_SERVICE_DEFINE(mn_hcp_svc,
    BT_GATT_PRIMARY_SERVICE(&SVC_UUID),
    /* HCP-CMD: host → node. Plain write — bonding is per-verb (§11.3.4). */
    BT_GATT_CHARACTERISTIC(&CMD_UUID.uuid,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE, NULL, cmd_write, NULL),
    /* HCP-EVT: node → host (attrs[4] is its value, [5] its CCC) */
    BT_GATT_CHARACTERISTIC(&EVT_UUID.uuid, BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ, evt_read, NULL, NULL),
    BT_GATT_CCC(evt_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    /* HCP-AUTH: read it to force pairing */
    BT_GATT_CHARACTERISTIC(&AUTH_UUID.uuid, BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ_ENCRYPT, auth_read, NULL, NULL),
);
#define EVT_ATTR (&mn_hcp_svc.attrs[4])

/* ---- outbound: chunk to (MTU-3), client reassembles at '\n' ---- */
void mn_ble_notify(const char *line) {
    struct bt_conn *conn = s_conn;
    if (!s_running || !conn ||
        !bt_gatt_is_subscribed(conn, EVT_ATTR, BT_GATT_CCC_NOTIFY)) return;
    size_t chunk = bt_gatt_get_mtu(conn) - 3;
    size_t len = strlen(line);
    char buf[256];
    if (chunk > sizeof(buf) - 1) chunk = sizeof(buf) - 1;

    for (size_t off = 0; off <= len; off += chunk) {
        size_t n = len - off;
        bool last = n <= chunk;
        if (!last) n = chunk;
        memcpy(buf, line + off, n);
        if (last) buf[n++] = '\n';            /* frame terminator */
        int rc = -ENOMEM;
        for (int attempt = 0; attempt < 12 && rc != 0; attempt++) {
            rc = bt_gatt_notify(conn, EVT_ATTR, buf, n);
            if (rc == -ENOMEM || rc == -ENOBUFS) k_msleep(10);
            else if (rc != 0) return;         /* link gone / not subscribed */
        }
        if (rc != 0 || last) return;
    }
}

/* ---- connection + security callbacks (BT RX thread: post, don't emit) ---- */
static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) return;
    s_conn = bt_conn_ref(conn);
    s_encrypted = false;
    mn_post_note("!STATE PROVISIONING ble-connected");
    /* Ask for security; per BLE-PAIRING.md centrals mostly ignore this and
     * pair on the HCP-AUTH read instead, but it costs nothing. */
    (void)bt_conn_set_security(conn, BT_SECURITY_L2);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    if (conn != s_conn) return;
    bt_conn_unref(s_conn);
    s_conn = NULL;
    s_encrypted = false;
    mn_post_note("# ble: disconnected (reason 0x%02x)", reason);
}

static void start_adv(void);

/* The connection object is free again: only now can connectable advertising
 * restart (doing it in disconnected() fails for lack of a conn slot). */
static void recycled(void) {
    if (s_running) start_adv();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err) {
    ARG_UNUSED(conn);
    s_encrypted = (err == BT_SECURITY_ERR_SUCCESS) && level >= BT_SECURITY_L2;
    mn_post_note("# ble: link %s (level=%d err=%d)",
                 s_encrypted ? "encrypted" : "NOT encrypted", level, err);
}

BT_CONN_CB_DEFINE(mn_conn_cb) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
    .security_changed = security_changed,
};

/* Numeric comparison can still be requested under Secure Connections even
 * though we have no I/O; a screenless node accepts (BLE-PAIRING.md §2). */
static void passkey_confirm(struct bt_conn *conn, unsigned int passkey) {
    ARG_UNUSED(passkey);
    bt_conn_auth_passkey_confirm(conn);
}
static void pairing_cancel(struct bt_conn *conn) { ARG_UNUSED(conn); }
static struct bt_conn_auth_cb s_auth = {
    .passkey_confirm = passkey_confirm,
    .cancel = pairing_cancel,
};

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx) {
    ARG_UNUSED(conn);
    mn_post_note("# ble: mtu negotiated tx=%u rx=%u", tx, rx);
}
static struct bt_gatt_cb s_gatt_cb = { .att_mtu_updated = mtu_updated };

/* ---- advertising: 128-bit service UUID in the advert, name in the scan
 * response (a host must be able to filter by service — BLE-PAIRING.md) ---- */
static const struct bt_data s_ad[] = {        /* file scope: BT_DATA_BYTES */
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, MN_UUID(0x000000000000)),
};

static void start_adv(void) {
    const char *name = bt_get_name();
    struct bt_data sd[] = { BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name)) };
    int rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, s_ad, ARRAY_SIZE(s_ad), sd, ARRAY_SIZE(sd));
    if (rc && rc != -EALREADY) mn_post_note("!WARN ble-adv-start rc=%d", rc);
}

int mn_ble_start(void) {
    if (s_running) return 0;
    static bool once;
    if (!once) {
        bt_conn_auth_cb_register(&s_auth);
        bt_gatt_cb_register(&s_gatt_cb);
        s_rx_q = xQueueCreate(6, MN_BLE_LINE_MAX);
        /* 6 KB like IDF's mn_ble_rx: BLE-fed verbs run here (incl. BUNDLE) */
        xTaskCreate(ble_worker, "mn_ble_rx", 6144, NULL, 5, NULL);
        once = true;
    }
    int rc = bt_enable(NULL);
    if (rc && rc != -EALREADY) return -1;
    /* bonds + identity live in settings under "bt" */
    settings_load_subtree("bt");

    char name[24];
    const uint8_t *id = mn_device_id();
    snprintf(name, sizeof(name), "MagNET-%02x%02x", id[2], id[3]);
    bt_set_name(name);

    s_running = true;
    start_adv();
#if MN_BLE_RESIDENT
    mn_post_note("# ble: advertising (resident — survives provisioning)");
#else
    mn_post_note("# ble: advertising for provisioning (torn down once a channel is set)");
#endif
    return 0;
}

int mn_ble_stop(void) {
    if (!s_running) return 0;
    s_running = false;
    bt_le_adv_stop();
    if (s_conn) bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    int rc = bt_disable();
    mn_post_note("# ble: torn down (channel provisioned); radio is Thread-only now");
    return rc;
}

bool mn_ble_running(void) { return s_running; }
bool mn_ble_link_secure(void) { return s_encrypted; }
