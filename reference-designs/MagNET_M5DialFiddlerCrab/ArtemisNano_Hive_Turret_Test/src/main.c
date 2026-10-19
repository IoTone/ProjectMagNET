/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ArtemisNano_Hive_Turret_Test, phase 2 bring-up.
 *
 * Gate: blue LED lights at boot, console prints the banner, LED then blinks.
 *   LED on, no text -> boot is fine, console pins/baud are wrong.
 *   LED off         -> the image never ran (load address, SVL jump, clocks).
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>
#include <cmsis_core.h>
#include <am_mcu_apollo.h>

#define BLUE_LED_PAD 19

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Runs before the kernel, drivers and console: drive the LED pad with the HAL. */
static int early_led_on(void)
{
	am_hal_gpio_pinconfig(BLUE_LED_PAD, g_AM_HAL_GPIO_OUTPUT);
	am_hal_gpio_output_set(BLUE_LED_PAD);
	return 0;
}
SYS_INIT(early_led_on, PRE_KERNEL_1, 0);

int main(void)
{
	printk("\n*** artemis_turret phase 2 on %s ***\n", CONFIG_BOARD_TARGET);
	printk("VTOR=0x%08x (expect 0x%08x)\n", (unsigned int)SCB->VTOR,
	       (unsigned int)CONFIG_FLASH_LOAD_OFFSET);

	if (!gpio_is_ready_dt(&led) || gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) < 0) {
		printk("led0 not ready\n");
		return 0;
	}

	for (uint32_t tick = 0;; tick++) {
		gpio_pin_toggle_dt(&led);
		if ((tick % 10) == 0) {
			printk("tick %u, uptime %lld ms\n", tick, k_uptime_get());
		}
		k_msleep(500);
	}
	return 0;
}
