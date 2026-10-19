/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include "rv1805.h"

#define REG_TRICKLE   0x20
#define REG_CONF_KEY  0x1F
#define REG_CAP_RC    0x26
#define REG_ID0       0x28

#define PART_ID0      0x18	/* AM1805 / RV-1805 part number, upper byte */
#define CONF_KEY_WRT  0x9D	/* unlocks trickle, BREF, CAPRC, batmode, output control */
#define CAP_RC_ENABLE 0xA0	/* enable the CAP_RC pin (supercap) */
#define TRICKLE_ON    0xA5	/* TCS=1010 (enable), diode 0.3 V, ROUT 3 kOhm */

int rv1805_board_init(const struct device *i2c, uint16_t addr)
{
	uint8_t id[2];
	uint8_t trickle;

	if (i2c_burst_read(i2c, addr, REG_ID0, id, sizeof(id)) != 0) {
		return -ENODEV;
	}
	printk("rv1805: ID0=0x%02x ID1=0x%02x\n", id[0], id[1]);
	if (id[0] != PART_ID0) {
		return -ENOTSUP;
	}

	/* CONF_KEY re-locks after each protected write, so unlock before each. */
	if (i2c_reg_write_byte(i2c, addr, REG_CONF_KEY, CONF_KEY_WRT) ||
	    i2c_reg_write_byte(i2c, addr, REG_CAP_RC, CAP_RC_ENABLE) ||
	    i2c_reg_write_byte(i2c, addr, REG_CONF_KEY, CONF_KEY_WRT) ||
	    i2c_reg_write_byte(i2c, addr, REG_TRICKLE, TRICKLE_ON)) {
		return -EIO;
	}

	if (i2c_reg_read_byte(i2c, addr, REG_TRICKLE, &trickle) == 0) {
		printk("rv1805: trickle=0x%02x (expect 0x%02x)\n", trickle, TRICKLE_ON);
	}
	return 0;
}
