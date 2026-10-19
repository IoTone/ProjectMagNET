/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ArtemisNano_Hive_Multisense.
 *
 * M2: RV-1805 RTC (via the ambiq,am1805 driver) and LC709203F fuel gauge.
 *   - Boot: Qwiic scan, RV-1805 part check + trickle charger on.
 *   - If the RTC has no valid time, set it from the build timestamp.
 *   - Every 10 s: print RTC time, battery voltage, state of charge.
 *
 * Boot check (from P2): blue LED lights before the kernel starts.
 *   LED on, no text -> console pins/baud wrong.  LED off -> image never ran.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <cmsis_core.h>
#include <am_mcu_apollo.h>
#include "rv1805.h"

#define BLUE_LED_PAD 19
#define RV1805_ADDR  0x69

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct device *const qwiic = DEVICE_DT_GET(DT_NODELABEL(qwiic_i2c));
static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
static const struct device *const gauge = DEVICE_DT_GET(DT_ALIAS(fuel_gauge0));

static const struct {
	uint8_t addr;
	const char *name;
} known[] = {
	{0x0B, "LC709203F fuel gauge"},
	{0x44, "OPT4048 color"},
	{0x45, "OPT4048 color (alt)"},
	{0x46, "OPT4048 color (alt)"},
	{0x4A, "BNO085 IMU"},
	{0x4B, "BNO085 IMU (alt)"},
	{0x69, "RV-1805 RTC"},
};

/* Runs before the kernel, drivers and console: drive the LED pad with the HAL. */
static int early_led_on(void)
{
	am_hal_gpio_pinconfig(BLUE_LED_PAD, g_AM_HAL_GPIO_OUTPUT);
	am_hal_gpio_output_set(BLUE_LED_PAD);
	return 0;
}
SYS_INIT(early_led_on, PRE_KERNEL_1, 0);

/*
 * Probe only the known Multisensor addresses: the I2C driver logs every NACK as an
 * error, so a full 0x08-0x77 sweep floods the console. This list becomes the
 * advert's sensor bitmap later (README 5.1).
 */
static void qwiic_scan(void)
{
	int found = 0;

	printk("qwiic probe:");
	for (size_t i = 0; i < ARRAY_SIZE(known); i++) {
		uint8_t b;

		/* A 1-byte read is the probe; all four parts tolerate it. */
		if (i2c_read(qwiic, &b, 1, known[i].addr) == 0) {
			printk("\n  0x%02x  %s", known[i].addr, known[i].name);
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

static void report(void)
{
	struct rtc_time now;

	if (device_is_ready(rtc) && rtc_get_time(rtc, &now) == 0) {
		printk("time %04d-%02d-%02d %02d:%02d:%02d", now.tm_year + 1900, now.tm_mon + 1,
		       now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
	} else {
		printk("time ?");
	}

	if (!device_is_ready(gauge)) {
		printk("  gauge not ready\n");
		return;
	}

	/* No TEMPERATURE: the Adafruit board has no thermistor, and the driver then
	 * fails the whole request with -ENOTSUP.
	 */
	fuel_gauge_prop_t props[] = {
		FUEL_GAUGE_VOLTAGE,
		FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE,
	};
	union fuel_gauge_prop_val vals[ARRAY_SIZE(props)];
	int err = fuel_gauge_get_props(gauge, props, vals, ARRAY_SIZE(props));

	if (err) {
		printk("  gauge err %d\n", err);
		return;
	}
	/* voltage in uV, RSOC in % */
	printk("  vbat %d.%03d V  soc %u%%\n", vals[0].voltage / 1000000,
	       (vals[0].voltage / 1000) % 1000, vals[1].relative_state_of_charge);
}

int main(void)
{
	printk("\n*** artemis_multisense M2 on %s ***\n", CONFIG_BOARD_TARGET);
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

	qwiic_scan();
	rtc_bringup();

	for (uint32_t tick = 0;; tick++) {
		gpio_pin_toggle_dt(&led);
		if ((tick % 20) == 0) {
			report();
		}
		k_msleep(500);
	}
	return 0;
}
