/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ArtemisNano_Hive_Multisense.
 *
 * M4: sensors (M2/M3) + BLE.
 *   - RV-1805 RTC (ambiq,am1805 driver + trickle charger), LC709203F fuel gauge,
 *     OPT4048 color (src/opt4048.c), BNO085 IMU via CEVA SH-2 (src/bno085.c).
 *   - Every 10 s: sample everything into one snapshot, print it, refresh the advert.
 *   - BLE (src/hive_ble.c): MagNET advert with the sensor bitmap + readings, and
 *     the Hanasu HCP GATT service answering WHOAMI/STATUS/CAPS/SENSE.
 *
 * Boot check (from P2): blue LED lights before the kernel starts.
 *   LED on, no text -> console pins/baud wrong.  LED off -> image never ran.
 *
 * Blue LED, once running:
 *   1 Hz even blink      -> up, advertising, nobody connected
 *   short pulse / 2.5 s  -> a central is connected
 *   solid on             -> never reached main() (see the boot check above)
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <cmsis_core.h>
#include <am_mcu_apollo.h>
#include "rv1805.h"
#include "opt4048.h"
#include "bno085.h"
#include "multisense.h"
#include "hive_ble.h"

#define BLUE_LED_PAD 19
#define RV1805_ADDR  0x69
#define OPT4048_ADDR 0x44
#define BNO085_ADDR  0x4A

/*
 * Full-bus sweep at boot, in addition to the targeted probe of known[].
 *
 * Off by default: it puts a 1-byte read on every one of the ~112 valid 7-bit
 * addresses, including ones no driver owns, and a working node does not need
 * that traffic on every boot. Turn it on when a part that should be present is
 * missing — it is the only way to see a device that moved address (an OPT4048
 * strapped to 0x45 instead of 0x44, say) or one that nobody has added to
 * known[] yet. known[] alone answers "is the Person Sensor there", so the sweep
 * is for the addresses we have not thought of.
 */
#define MS_BUS_SWEEP 0

#define REPORT_PERIOD_MS 10000

/*
 * Blue LED cadence — the only thing this board can say without a console, so
 * the two states must not look alike from across the bench:
 *   advertising, nobody connected : 1 Hz even blink (as it has always been)
 *   a central connected           : a short pulse every 2.5 s
 * The connected pattern is also the cheaper one (80 ms on in 2500 ms, ~3% duty
 * against 50%), which matters for M5: the red power LED is ~1.5 mA and this one
 * is on the same order.
 */
#define LED_ADV_ON_MS    500
#define LED_ADV_OFF_MS   500
#define LED_CONN_ON_MS    80
#define LED_CONN_OFF_MS 2420

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct device *const qwiic = DEVICE_DT_GET(DT_NODELABEL(qwiic_i2c));
static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
static const struct device *const gauge = DEVICE_DT_GET(DT_ALIAS(fuel_gauge0));

static const struct {
	uint8_t addr;
	const char *name;
	uint16_t caps;
} known[] = {
	{0x0B, "LC709203F fuel gauge", MS_CAP_BATTERY},
	{0x44, "OPT4048 color", MS_CAP_LUX | MS_CAP_COLOR},
	{0x45, "OPT4048 color (alt)", 0},
	{0x46, "OPT4048 color (alt)", 0},
	{0x4A, "BNO085 IMU", MS_CAP_ORIENTATION},
	{0x4B, "BNO085 IMU (alt)", 0},
	/*
	 * Useful Sensors Person Sensor. v1.0, v1.1 and v2.0 all answer here and are
	 * software-compatible. The M1 scan (2026-10-19) saw an ACK at 0x62 and
	 * recorded it as an unexpected address from a faulty AK9753 board; 0x62 is
	 * this part's documented address, so it was most likely answering correctly.
	 *
	 * caps stays 0 on purpose: there is no driver yet, and setting
	 * MS_CAP_PRESENCE with nothing to read it would advertise a sensor that
	 * never reports a reading.
	 */
	{0x62, "Person Sensor (Useful Sensors)", 0},
	{0x69, "RV-1805 RTC", MS_CAP_TIME},
};

static K_MUTEX_DEFINE(snap_lock);
static struct ms_readings snap = {.soc_pct = 0xFF, .tbat_dc = TBAT_NONE_DC};
static uint16_t probed_caps;
static uint8_t device_id[4];

/* Runs before the kernel, drivers and console: drive the LED pad with the HAL. */
static int early_led_on(void)
{
	am_hal_gpio_pinconfig(BLUE_LED_PAD, g_AM_HAL_GPIO_OUTPUT);
	am_hal_gpio_output_set(BLUE_LED_PAD);
	return 0;
}
SYS_INIT(early_led_on, PRE_KERNEL_1, 0);

void ms_readings_get(struct ms_readings *out)
{
	k_mutex_lock(&snap_lock, K_FOREVER);
	*out = snap;
	k_mutex_unlock(&snap_lock);
}

const uint8_t *ms_device_id(void)
{
	return device_id;
}

/* FNV-1a over the two 32-bit chip ID words: stable per part, spreads low-bit differences. */
static void device_id_init(void)
{
	am_hal_mcuctrl_device_t dev;
	uint32_t h = 2166136261u;

	am_hal_mcuctrl_info_get(AM_HAL_MCUCTRL_INFO_DEVICEID, &dev);
	uint32_t words[2] = {dev.ui32ChipID0, dev.ui32ChipID1};

	for (int w = 0; w < 2; w++) {
		for (int b = 0; b < 4; b++) {
			h ^= (words[w] >> (8 * b)) & 0xFF;
			h *= 16777619u;
		}
	}
	sys_put_be32(h, device_id);
	printk("chip id %08x %08x -> node id %02x%02x%02x%02x\n", dev.ui32ChipID0,
	       dev.ui32ChipID1, device_id[0], device_id[1], device_id[2], device_id[3]);
}

/*
 * Probe only the known Multisensor addresses: the I2C driver logs every NACK as an
 * error, so a full 0x08-0x77 sweep floods the console. Answers set the advert's
 * sensor bitmap; a driver that then fails to start clears its bit again.
 */
#if MS_BUS_SWEEP
/* 0x00-0x07 and 0x78-0x7F are reserved by the I2C spec, so they are skipped. */
static void qwiic_sweep(void)
{
	int found = 0;

	printk("qwiic sweep (0x08-0x77):");
	for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
		uint8_t b;

		if (i2c_read(qwiic, &b, 1, addr) != 0) {
			continue;
		}

		const char *name = "not in known[]";

		for (size_t i = 0; i < ARRAY_SIZE(known); i++) {
			if (known[i].addr == addr) {
				name = known[i].name;
				break;
			}
		}
		printk("\n  0x%02x  %s", addr, name);
		found++;
	}
	printk(found ? "\n  %d device(s)\n" : " nothing\n", found);
}
#endif /* MS_BUS_SWEEP */

static void qwiic_scan(void)
{
	int found = 0;

	printk("qwiic probe:");
	for (size_t i = 0; i < ARRAY_SIZE(known); i++) {
		uint8_t b;

		/* A 1-byte read is the probe; all four parts tolerate it. */
		if (i2c_read(qwiic, &b, 1, known[i].addr) == 0) {
			printk("\n  0x%02x  %s", known[i].addr, known[i].name);
			probed_caps |= known[i].caps;
			found++;
		}
	}
	printk(found ? "\n  %d device(s)\n" : " no devices\n", found);
}

/* Build timestamp -> rtc_time, used once when the RTC holds no valid time. */
static void build_time(struct rtc_time *t)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char mon[4] = {0};
	int day = 1, year = 2026, h = 0, m = 0, s = 0;
	const char *p;

	sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
	sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
	p = strstr(months, mon);

	memset(t, 0, sizeof(*t));
	t->tm_year = year - 1900;
	t->tm_mon = p ? (int)((p - months) / 3) : 0;
	t->tm_mday = day;
	t->tm_hour = h;
	t->tm_min = m;
	t->tm_sec = s;
	t->tm_wday = -1;
	t->tm_yday = -1;
	t->tm_isdst = -1;
}

static void rtc_bringup(void)
{
	struct rtc_time now;
	int err;

	err = rv1805_board_init(qwiic, RV1805_ADDR);
	if (err) {
		printk("rv1805: board init failed (%d)\n", err);
	}
	if (!device_is_ready(rtc)) {
		printk("rtc: driver not ready\n");
		probed_caps &= ~MS_CAP_TIME;
		return;
	}

	err = rtc_get_time(rtc, &now);
	if (err || now.tm_year + 1900 < 2026) {
		struct rtc_time t;

		build_time(&t);
		printk("rtc: no valid time (err %d, year %d); setting build time\n", err,
		       err ? 0 : now.tm_year + 1900);
		err = rtc_set_time(rtc, &t);
		if (err) {
			printk("rtc: set failed (%d)\n", err);
		}
	} else {
		printk("rtc: kept time across reset\n");
	}
}

static void sensors_bringup(void)
{
	int err = opt4048_init(qwiic, OPT4048_ADDR);

	if (err) {
		printk("opt4048: init failed (%d)\n", err);
		probed_caps &= ~(MS_CAP_LUX | MS_CAP_COLOR);
	}

	err = bno085_start(qwiic, BNO085_ADDR);
	if (err) {
		printk("bno085: start failed (%d)\n", err);
		probed_caps &= ~MS_CAP_ORIENTATION;
	}
	if (!device_is_ready(gauge)) {
		probed_caps &= ~MS_CAP_BATTERY;
	}
}

/* Always TBAT_NONE_DC until a thermistor is fitted (then: FUEL_GAUGE_TEMPERATURE
 * with TSENSE enabled in the driver).
 */
static int16_t battery_temp_dc(void)
{
	return TBAT_NONE_DC;
}

/* Read every sensor into a fresh snapshot, then publish it under the lock. */
static void sample(void)
{
	struct ms_readings r = {
		.caps = probed_caps,
		.soc_pct = 0xFF,
		.tbat_dc = battery_temp_dc(),
	};
	struct rtc_time now;

	k_mutex_lock(&snap_lock, K_FOREVER);
	r.seq = snap.seq + 1;
	k_mutex_unlock(&snap_lock);

	if ((r.caps & MS_CAP_TIME) && rtc_get_time(rtc, &now) == 0) {
		/* Field ranges bounded so the compiler can prove the 19-character fit. */
		snprintf(r.time_iso, sizeof(r.time_iso), "%04u-%02u-%02uT%02u:%02u:%02u",
			 (now.tm_year + 1900) % 10000u, (now.tm_mon + 1) % 100u,
			 now.tm_mday % 100u, now.tm_hour % 100u, now.tm_min % 100u,
			 now.tm_sec % 100u);
		r.time_ok = true;
	}

	if (r.caps & MS_CAP_BATTERY) {
		/* No FUEL_GAUGE_TEMPERATURE: with no thermistor the driver fails the whole
		 * request with -ENOTSUP. battery_temp_dc() stands in for it.
		 */
		fuel_gauge_prop_t props[] = {
			FUEL_GAUGE_VOLTAGE,
			FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE,
		};
		union fuel_gauge_prop_val vals[ARRAY_SIZE(props)];

		if (fuel_gauge_get_props(gauge, props, vals, ARRAY_SIZE(props)) == 0) {
			r.vbat_mv = vals[0].voltage / 1000;	/* uV -> mV */
			r.soc_pct = vals[1].relative_state_of_charge;
		}
	}

	if (r.caps & MS_CAP_LUX) {
		struct opt4048_sample s;

		if (opt4048_read(qwiic, OPT4048_ADDR, &s) == 0) {
			r.light_ok = true;
			r.lux = s.lux;
			r.cie_x = s.cie_x;
			r.cie_y = s.cie_y;
			r.cct = s.cct;
		}
	}

	if (r.caps & MS_CAP_ORIENTATION) {
		struct bno085_sample s;

		bno085_get(&s);
		if (s.valid) {
			r.imu_ok = true;
			r.yaw = s.yaw;
			r.pitch = s.pitch;
			r.roll = s.roll;
			r.heading_acc_deg = s.accuracy_deg;
			r.stability = s.stability;
			r.sig_motion = s.sig_motion;
		}
	}

	k_mutex_lock(&snap_lock, K_FOREVER);
	snap = r;
	k_mutex_unlock(&snap_lock);
}

static void print_snapshot(void)
{
	struct ms_readings r;

	ms_readings_get(&r);
	printk("#%u time %s", r.seq, r.time_ok ? r.time_iso : "?");
	if (r.caps & MS_CAP_BATTERY) {
		printk("  vbat %u.%03u V  soc %u%%", r.vbat_mv / 1000, r.vbat_mv % 1000, r.soc_pct);
		if (r.tbat_dc == TBAT_NONE_DC) {
			printk("  tbat n/a (no thermistor)");
		} else {
			printk("  tbat %d.%d C", r.tbat_dc / 10, r.tbat_dc % 10);
		}
	}
	printk("\n");

	if (r.light_ok) {
		printk("light  %.1f lux  xy %.4f,%.4f  cct %.0f K\n", (double)r.lux,
		       (double)r.cie_x, (double)r.cie_y, (double)r.cct);
	} else {
		printk("light  %s\n", (r.caps & MS_CAP_LUX) ? "no data" : "absent");
	}

	if (r.imu_ok) {
		printk("imu    yaw %.1f pitch %.1f roll %.1f (+/-%.1f deg)  %s  sigmo=%u\n",
		       (double)r.yaw, (double)r.pitch, (double)r.roll, (double)r.heading_acc_deg,
		       bno085_stability_str(r.stability), r.sig_motion);
	} else {
		printk("imu    %s\n", (r.caps & MS_CAP_ORIENTATION) ? "no data yet" : "absent");
	}
}

int main(void)
{
	printk("\n*** artemis_multisense " MS_FW_VERSION " on %s ***\n", CONFIG_BOARD_TARGET);
	printk("VTOR=0x%08x (expect 0x%08x)\n", (unsigned int)SCB->VTOR,
	       (unsigned int)CONFIG_FLASH_LOAD_OFFSET);

	if (!gpio_is_ready_dt(&led) || gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) < 0) {
		printk("led0 not ready\n");
		return 0;
	}
	if (!device_is_ready(qwiic)) {
		printk("qwiic_i2c not ready\n");
		return 0;
	}

	device_id_init();
#if MS_BUS_SWEEP
	qwiic_sweep();
#endif
	qwiic_scan();
	rtc_bringup();
	sensors_bringup();
	printk("sensor bitmap 0x%04x\n", probed_caps);

	sample();
	if (hive_ble_start() != 0) {
		printk("ble: not started; sensors only\n");
	}

	/*
	 * Two independent deadlines on one thread: the report period is wall-clock
	 * and must not drift when the LED slows down on connect, and an LED phase
	 * must not stretch because a sample happened to land inside it.
	 */
	int64_t next_sample = k_uptime_get() + REPORT_PERIOD_MS;
	int64_t next_led = k_uptime_get() + LED_ADV_ON_MS;
	bool led_on = true;	/* already on: GPIO_OUTPUT_ACTIVE, and early_led_on() */

	for (;;) {
		const int64_t now = k_uptime_get();

		if (now >= next_sample) {
			sample();
			print_snapshot();
			hive_ble_update();
			next_sample += REPORT_PERIOD_MS;
			continue;
		}

		if (now >= next_led) {
			led_on = !led_on;
			gpio_pin_set_dt(&led, led_on ? 1 : 0);
			/*
			 * The cadence is read at each edge, so a connect or disconnect
			 * shows up within one phase (at worst the 2.4 s off-phase).
			 */
			const bool conn = hive_ble_connected();

			if (led_on) {
				next_led = now + (conn ? LED_CONN_ON_MS : LED_ADV_ON_MS);
			} else {
				next_led = now + (conn ? LED_CONN_OFF_MS : LED_ADV_OFF_MS);
			}
			continue;
		}

		k_msleep((uint32_t)(MIN(next_sample, next_led) - now));
	}
	return 0;
}
