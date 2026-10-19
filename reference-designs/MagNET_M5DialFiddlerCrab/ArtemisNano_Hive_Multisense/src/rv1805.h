/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zephyr/device.h>

/*
 * RV-1805 setup the upstream ambiq,am1805 driver does not do: verify the part
 * number and enable the supercap trickle charger (otherwise the backup cap never
 * charges and time is lost on power-off). Values from SparkFun_RV-1805 begin().
 * Returns 0, -ENODEV (no answer), -ENOTSUP (wrong part) or -EIO.
 */
int rv1805_board_init(const struct device *i2c, uint16_t addr);
