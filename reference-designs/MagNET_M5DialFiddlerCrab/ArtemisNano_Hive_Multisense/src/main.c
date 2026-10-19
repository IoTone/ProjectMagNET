/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ArtemisNano_Hive_Multisense.
 *
 * M1: Qwiic bus scan every 10 s, so breakouts show up as they are plugged in.
 * Expected once all four are chained: 0x0B (LC709203F), 0x44 (OPT4048),
 * 0x64 (AK9753), 0x69 (RV-1805).
 *
 * Boot check (from P2): blue LED lights before the kernel starts.
 *   LED on, no text -> console pins/baud wrong.  LED off -> image never ran.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <cmsis_core.h>
#include <am_mcu_apollo.h>

#define BLUE_LED_PAD 19

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct device *const qwiic = DEVICE_DT_GET(DT_NODELABEL(qwiic_i2c));

static const struct {
	uint8_t addr;
	const char *name;
} known[] = {
	{0x0B, "LC709203F fuel gauge"},
	{0x44, "OPT4048 color"},
	{0x45, "OPT4048 color (alt)"},
	{0x46, "OPT4048 color (alt)"},
	{0x64, "AK9753 presence"},
	{0x65, "AK9753 presence (alt)"},
	{0x67, "AK9753 presence (alt)"},
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

static const char *known_name(uint8_t addr)
{
	for (size_t i = 0; i < ARRAY_SIZE(known); i++) {
		if (known[i].addr == addr) {
			return known[i].name;
		}
	}
	return "unknown";
}

static void qwiic_scan(void)
{
	int found = 0;

	printk("qwiic scan:");
	for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
		uint8_t b;

		/* A 1-byte read is the probe; all four parts tolerate it. */
		if (i2c_read(qwiic, &b, 1, addr) == 0) {
			printk("\n  0x%02x  %s", addr, known_name(addr));
			found++;
		}
	}
	printk(found ? "\n  %d device(s)\n" : " no devices\n", found);
}

int main(void)
{
	printk("\n*** artemis_multisense M1 on %s ***\n", CONFIG_BOARD_TARGET);
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

	for (uint32_t tick = 0;; tick++) {
		gpio_pin_toggle_dt(&led);
		if ((tick % 20) == 0) {
			qwiic_scan();
		}
		k_msleep(500);
	}
	return 0;
}
