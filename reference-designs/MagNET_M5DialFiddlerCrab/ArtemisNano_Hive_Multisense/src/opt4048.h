/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <zephyr/device.h>

/*
 * TI OPT4048 tristimulus color sensor (Adafruit 6335). Zephyr has no driver, so
 * this is a small register-level one: continuous auto-range conversions, burst
 * read of all four channels with CRC check, CIE 1931 xy + lux + CCT from the
 * datasheet matrix (same coefficients as Adafruit_OPT4048).
 */
struct opt4048_sample {
	uint32_t raw[4];	/* ADC codes CH0..CH3 (X, Y, Z, W) */
	uint8_t counter;	/* CH0 conversion counter, 0-15, wraps; increments per conversion */
	float lux;
	float cie_x;
	float cie_y;
	float cct;		/* Kelvin, 0 when the light is too dim to compute */
};

/* Returns 0, -ENODEV (no answer), -ENOTSUP (wrong device ID) or -EIO. */
int opt4048_init(const struct device *i2c, uint16_t addr);

/*
 * Returns 0, -EBADMSG (CRC mismatch) or -EIO.
 *
 * **Zero is a valid reading.** A covered or unlit sensor really does report
 * zero on all four channels, so this no longer reports that as -EAGAIN: doing
 * so made darkness indistinguishable from a broken part, and the node then
 * published "no light sensor data" instead of "0 lux". opt4048_init() waits out
 * the first conversion so a read is always against converted data.
 */
int opt4048_read(const struct device *i2c, uint16_t addr, struct opt4048_sample *s);
