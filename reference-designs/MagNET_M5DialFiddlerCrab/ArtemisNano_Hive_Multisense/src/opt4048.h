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
	float lux;
	float cie_x;
	float cie_y;
	float cct;		/* Kelvin, 0 when the light is too dim to compute */
};

/* Returns 0, -ENODEV (no answer), -ENOTSUP (wrong device ID) or -EIO. */
int opt4048_init(const struct device *i2c, uint16_t addr);

/* Returns 0, -EAGAIN (no conversion yet), -EBADMSG (CRC mismatch) or -EIO. */
int opt4048_read(const struct device *i2c, uint16_t addr, struct opt4048_sample *s);
