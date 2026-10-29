/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include "multisense.h"
#include "hive_ble.h"

#define COMPANY_TEST 0xFFFF
#define ADV_FORMAT   1
#define HDR_LEN      6
#define RSP_LEN      (HDR_LEN + 8)

/* 6d61676e-2d68-6370-0001-0000000000NN: ASCII "magn" "-h" "cp" (Hanasu) */
#define MN_UUID(nn) BT_UUID_128_ENCODE(0x6d61676e, 0x2d68, 0x6370, 0x0001, (nn##ULL))
static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(MN_UUID(0x000000000000));
static const struct bt_uuid_128 cmd_uuid = BT_UUID_INIT_128(MN_UUID(0x000000000001));
static const struct bt_uuid_128 evt_uuid = BT_UUID_INIT_128(MN_UUID(0x000000000002));
static const struct bt_uuid_128 auth_uuid = BT_UUID_INIT_128(MN_UUID(0x000000000003));

static struct bt_conn *cur_conn;
static bool encrypted;
static bool running;
static char name[16];

/* ---- advert payloads ---- */

static uint8_t adv_mfg[2 + HDR_LEN];
static uint8_t rsp_mfg[2 + RSP_LEN];

static const uint8_t adv_flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
static const uint8_t adv_uuid[] = {MN_UUID(0x000000000000)};

static struct bt_data ad[] = {
	BT_DATA(BT_DATA_FLAGS, &adv_flags, 1),
	BT_DATA(BT_DATA_UUID128_ALL, adv_uuid, sizeof(adv_uuid)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, adv_mfg, sizeof(adv_mfg)),
};
static struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, name, 0),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, rsp_mfg, sizeof(rsp_mfg)),
};

static uint16_t sat_u16(float v)
{
	return v <= 0.0f ? 0 : (v >= 65535.0f ? 65535 : (uint16_t)(v + 0.5f));
}

static void build_payloads(void)
{
	struct ms_readings r;

	ms_readings_get(&r);

	uint8_t *h = &rsp_mfg[2];

	sys_put_le16(COMPANY_TEST, rsp_mfg);
	h[0] = (ADV_FORMAT << 4) | MS_ROLE_MULTISENSOR;
	sys_put_le16(r.caps, &h[1]);
	h[3] = (r.caps & MS_CAP_BATTERY) ? r.soc_pct : 0xFF;
	h[4] = r.seq;
	h[5] = 0;	/* no role bundle installed yet (M7) */

	sys_put_le16(r.light_ok ? sat_u16(r.lux) : 0xFFFF, &h[6]);
	sys_put_le16(r.light_ok ? sat_u16(r.cct) : 0xFFFF, &h[8]);
	sys_put_le16(r.imu_ok ? (uint16_t)(int16_t)r.yaw : 0x8000, &h[10]);
	h[12] = r.imu_ok ? r.stability : 0xFF;
	h[13] = (uint8_t)r.sig_motion;

	memcpy(adv_mfg, rsp_mfg, sizeof(adv_mfg));
}

bool hive_ble_connected(void)
{
	return cur_conn != NULL;
}

static void start_adv(void)
{
	int err;

	build_payloads();
	sd[0].data_len = strlen(name);
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err && err != -EALREADY) {
		printk("ble: adv start failed (%d)\n", err);
	}
}

void hive_ble_update(void)
{
	if (!running) {
		return;
	}
	build_payloads();
	/* -EAGAIN while connected (not advertising): ignored, refreshed on restart. */
	(void)bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
}

/* ---- GATT: Hanasu HCP service ---- */

#define LINE_MAX 200
K_MSGQ_DEFINE(line_q, LINE_MAX, 4, 4);
static char rx_line[LINE_MAX];
static size_t rx_len;

static void post_line(void)
{
	if (rx_len == 0) {
		return;
	}
	rx_line[rx_len] = '\0';
	(void)k_msgq_put(&line_q, rx_line, K_NO_WAIT);	/* never block the BT RX thread */
	rx_len = 0;
}

static void feed(const uint8_t *data, size_t len, bool write_complete)
{
	for (size_t i = 0; i < len; i++) {
		char c = (char)data[i];

		if (c == '\n' || c == '\r') {
			post_line();
			write_complete = false;
		} else if (rx_len < sizeof(rx_line) - 1) {
			rx_line[rx_len++] = c;
		}
	}
	if (write_complete) {
		post_line();
	}
}

static void long_write_flush(struct k_work *w)
{
	ARG_UNUSED(w);
	post_line();
}
static K_WORK_DELAYABLE_DEFINE(long_flush, long_write_flush);

static ssize_t cmd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
		return 0;
	}
	if (flags & BT_GATT_WRITE_FLAG_EXECUTE) {
		if (offset == 0) {
			rx_len = 0;
		}
		feed(buf, len, false);
		k_work_reschedule(&long_flush, K_MSEC(20));
		return len;
	}
	/* A write of exactly (MTU-3) may continue; anything else ends the line. */
	feed(buf, len, len != bt_gatt_get_mtu(conn) - 3);
	return len;
}

static ssize_t auth_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	static const char ok[] = "bonded";

	return bt_gatt_attr_read(conn, attr, buf, len, offset, ok, sizeof(ok) - 1);
}

static ssize_t evt_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, "", 0);
}

static void evt_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(value);
}

BT_GATT_SERVICE_DEFINE(hcp_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&cmd_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE, NULL, cmd_write,
			       NULL),
	BT_GATT_CHARACTERISTIC(&evt_uuid.uuid, BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, evt_read, NULL, NULL),
	BT_GATT_CCC(evt_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&auth_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
			       auth_read, NULL, NULL),
);
#define EVT_ATTR (&hcp_svc.attrs[4])

/* Chunk to (MTU-3); the client reassembles at '\n'. Also echoed to the console. */
static void write_line(const char *line)
{
	struct bt_conn *conn = cur_conn;
	char buf[256];

	printk("hcp> %s\n", line);
	if (!conn || !bt_gatt_is_subscribed(conn, EVT_ATTR, BT_GATT_CCC_NOTIFY)) {
		return;
	}

	size_t chunk = MIN(bt_gatt_get_mtu(conn) - 3, sizeof(buf) - 1);
	size_t len = strlen(line);

	for (size_t off = 0; off <= len; off += chunk) {
		size_t n = len - off;
		/* Strictly less: the '\n' must fit in the same notification. A line of
		 * exactly `chunk` bytes goes out full, then the '\n' alone next pass.
		 */
		bool last = n < chunk;
		int rc = -ENOMEM;

		if (!last) {
			n = chunk;
		}
		memcpy(buf, line + off, n);
		if (last) {
			buf[n++] = '\n';
		}
		for (int attempt = 0; attempt < 12 && rc != 0; attempt++) {
			rc = bt_gatt_notify(conn, EVT_ATTR, buf, n);
			if (rc == -ENOMEM || rc == -ENOBUFS) {
				k_msleep(10);
			} else if (rc != 0) {
				printk("ble: notify failed (%d) at %u/%u\n", rc, off, len);
				return;
			}
		}
		if (rc != 0 || last) {
			return;
		}
	}
}

/* ---- minimal HCP dispatcher (until the shared core arrives in M7) ---- */

static void respond(const char *tag, const char *body)
{
	char out[320];

	if (tag[0]) {
		snprintf(out, sizeof(out), "@%s %s", tag, body);
	} else {
		snprintf(out, sizeof(out), "%s", body);
	}
	write_line(out);
}

static const char *stability_str(uint8_t s)
{
	static const char *const names[] = {"unknown", "on-table", "stationary", "stable",
					    "motion"};

	return s < ARRAY_SIZE(names) ? names[s] : "?";
}

static void sense_line(char *buf, size_t cap)
{
	struct ms_readings r;
	int n;

	ms_readings_get(&r);
	n = snprintf(buf, cap, "+OK seq=%u", r.seq);
	if (r.time_ok) {
		n += snprintf(buf + n, cap - n, " time=%s", r.time_iso);
	}
	if (r.caps & MS_CAP_BATTERY) {
		n += snprintf(buf + n, cap - n, " vbat=%u.%03u soc=%u", r.vbat_mv / 1000,
			      r.vbat_mv % 1000, r.soc_pct);
		n += snprintf(buf + n, cap - n, r.tbat_dc == TBAT_NONE_DC ? " tbat=na" : " tbat=%d.%d",
			      r.tbat_dc / 10, r.tbat_dc % 10);
	}
	if (r.light_ok) {
		n += snprintf(buf + n, cap - n, " lux=%.1f cct=%.0f x=%.4f y=%.4f", (double)r.lux,
			      (double)r.cct, (double)r.cie_x, (double)r.cie_y);
	}
	if (r.imu_ok) {
		snprintf(buf + n, cap - n, " yaw=%.1f pitch=%.1f roll=%.1f hacc=%.0f motion=%s sigmo=%u",
			 (double)r.yaw, (double)r.pitch, (double)r.roll, (double)r.heading_acc_deg,
			 stability_str(r.stability), r.sig_motion);
	}
}

static void caps_line(char *buf, size_t cap)
{
	struct ms_readings r;
	int n;

	ms_readings_get(&r);
	n = snprintf(buf, cap,
		     "+OK proto=2.1 fw=" MS_FW_VERSION " maxline=%d transports=ble "
		     "verbs=PING,HELP,WHOAMI,STATUS,CAPS,SENSE,CHANNEL,ADMIN mode=HCP role=multisensor sensors=",
		     LINE_MAX);
	const char *sep = "";

	if (r.caps & MS_CAP_ORIENTATION) {
		n += snprintf(buf + n, cap - n, "%sorientation:bno085,motion:bno085", sep);
		sep = ",";
	}
	if (r.caps & MS_CAP_LUX) {
		n += snprintf(buf + n, cap - n, "%slux:opt4048,cct:opt4048", sep);
		sep = ",";
	}
	if (r.caps & MS_CAP_TIME) {
		n += snprintf(buf + n, cap - n, "%stime:rv1805", sep);
		sep = ",";
	}
	if (r.caps & MS_CAP_BATTERY) {
		snprintf(buf + n, cap - n, "%sbattery:lc709203f", sep);
	}
}

static void dispatch(char *line)
{
	char tag[16] = "";
	char body[320];
	const uint8_t *id = ms_device_id();
	char *p = line;

	while (*p == ' ') {
		p++;
	}
	if (*p == '@') {
		size_t i = 0;

		p++;
		while (*p && *p != ' ' && i < sizeof(tag) - 1) {
			tag[i++] = *p++;
		}
		tag[i] = '\0';
		while (*p == ' ') {
			p++;
		}
	}
	char *verb = p;

	while (*p && *p != ' ') {
		p++;
	}
	char *rest = p;

	if (*rest == ' ') {
		*rest++ = '\0';
		while (*rest == ' ') {
			rest++;
		}
	}

	if (verb[0] == '\0') {
		respond(tag, "-ERR E_SYNTAX empty");
	} else if (!strcmp(verb, "PING")) {
		respond(tag, "+PONG");
	} else if (!strcmp(verb, "HELP")) {
		write_line("# HCP verbs: PING HELP WHOAMI STATUS CAPS SENSE  CHANNEL SHOW  ADMIN LIST");
		respond(tag, "+OK");
	} else if (!strcmp(verb, "WHOAMI")) {
		snprintf(body, sizeof(body), "+OK id=%02x%02x%02x%02x name=%s fw=" MS_FW_VERSION,
			 id[0], id[1], id[2], id[3], name);
		respond(tag, body);
	} else if (!strcmp(verb, "STATUS")) {
		snprintf(body, sizeof(body),
			 "+OK state=READY role=multisensor channel=none peers=0 name=%s "
			 "id=%02x%02x%02x%02x ble=up link=%s",
			 name, id[0], id[1], id[2], id[3], encrypted ? "encrypted" : "open");
		respond(tag, body);
	} else if (!strcmp(verb, "CAPS")) {
		caps_line(body, sizeof(body));
		respond(tag, body);
	} else if (!strcmp(verb, "SENSE")) {
		sense_line(body, sizeof(body));
		respond(tag, body);
	} else if (!strcmp(verb, "CHANNEL")) {
		/* No mesh channel on this node yet (BLE-only until M7). magnet_app's node
		 * screen reads CHANNEL SHOW on every refresh, so answer it honestly.
		 */
		if (!strncmp(rest, "SHOW", 4) || !strncmp(rest, "LIST", 4)) {
			respond(tag, "+OK name=none selector=- path=-");
		} else {
			respond(tag, "-ERR E_UNSUPPORTED no mesh channel on this node yet");
		}
	} else if (!strcmp(verb, "ADMIN")) {
		/* Empty allow-list: no privileged verbs exist here yet. */
		if (!strncmp(rest, "LIST", 4)) {
			respond(tag, "+OK");
		} else {
			respond(tag, "-ERR E_UNSUPPORTED no privileged verbs on this node yet");
		}
	} else {
		snprintf(body, sizeof(body), "-ERR E_UNKNOWN %s", verb);
		respond(tag, body);
	}
}

static void hcp_thread(void *p1, void *p2, void *p3)
{
	char line[LINE_MAX];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		k_msgq_get(&line_q, line, K_FOREVER);
		printk("hcp< %s\n", line);
		dispatch(line);
	}
}
K_THREAD_DEFINE(hcp_tid, 2048, hcp_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(9), 0, 0);

/* ---- connection + security ---- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		printk("ble: connect failed (0x%02x)\n", err);
		return;
	}
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("ble: connected %s\n", addr);
	cur_conn = bt_conn_ref(conn);
	encrypted = false;
	/* No Security Request here (unlike Hanasu): Windows turns it into a pairing
	 * prompt that blocks the first write, and every verb so far is read-only.
	 * A host that wants an encrypted link reads HCP-AUTH, which forces pairing.
	 */
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (conn != cur_conn) {
		return;
	}
	printk("ble: disconnected (reason 0x%02x)\n", reason);
	bt_conn_unref(cur_conn);
	cur_conn = NULL;
	encrypted = false;
}

/* Restart advertising only once the connection object is free again. */
static void recycled(void)
{
	if (running) {
		start_adv();
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	ARG_UNUSED(conn);
	encrypted = (err == BT_SECURITY_ERR_SUCCESS) && level >= BT_SECURITY_L2;
	printk("ble: link %s (level %d err %d)\n", encrypted ? "encrypted" : "NOT encrypted",
	       level, err);
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

/* Screenless node: accept numeric comparison (Just Works in effect). */
static void passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	ARG_UNUSED(passkey);
	bt_conn_auth_passkey_confirm(conn);
}

static void pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
}

static struct bt_conn_auth_cb auth_cb = {
	.passkey_confirm = passkey_confirm,
	.cancel = pairing_cancel,
};

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(conn);
	printk("ble: mtu tx=%u rx=%u\n", tx, rx);
}

static struct bt_gatt_cb gatt_cb = {.att_mtu_updated = mtu_updated};

int hive_ble_start(void)
{
	const uint8_t *id = ms_device_id();
	bt_addr_le_t addrs[1];
	size_t count = 1;
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_conn_auth_cb_register(&auth_cb);
	bt_gatt_cb_register(&gatt_cb);

	err = bt_enable(NULL);
	if (err) {
		printk("ble: bt_enable failed (%d)\n", err);
		return err;
	}
	settings_load_subtree("bt");

	snprintf(name, sizeof(name), "MagNET-%02x%02x", id[2], id[3]);
	bt_set_name(name);

	bt_id_get(addrs, &count);
	if (count) {
		bt_addr_le_to_str(&addrs[0], addr, sizeof(addr));
		printk("ble: %s advertising as %s\n", name, addr);
	}

	running = true;
	start_adv();
	return 0;
}
