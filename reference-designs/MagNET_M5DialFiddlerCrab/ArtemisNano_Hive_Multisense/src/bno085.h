/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

/*
 * BNO085 9-DoF fusion IMU (Adafruit 4754) over I2C, via CEVA's SH-2 library
 * (lib/sh2, Apache-2.0). A dedicated thread owns the SH-2 instance and polls the
 * bus every 10 ms; INT (D8) and RST (D10) are not required yet. Once INT is wired
 * the thread will sleep until it fires instead (M5).
 *
 * Reports enabled: rotation vector (10 Hz), stability classifier (1 Hz) and
 * significant motion (one-shot; re-armed after each event).
 */
struct bno085_sample {
	bool valid;		/* at least one rotation vector received */
	float qi, qj, qk, qr;	/* unit quaternion */
	float accuracy_deg;	/* heading accuracy estimate */
	float yaw, pitch, roll;	/* degrees */
	uint8_t stability;	/* 0 unknown, 1 on table, 2 stationary, 3 stable, 4 motion */
	uint32_t reports;	/* rotation vector reports since boot */
	uint32_t resets;	/* hub resets seen (includes the boot reset) */
	uint32_t sig_motion;	/* significant-motion events since boot */
};

/* Start the IMU thread. Returns 0, or -ENODEV if the hub never answered. */
int bno085_start(const struct device *i2c, uint16_t addr);

/* Copy the latest values. */
void bno085_get(struct bno085_sample *out);

/* Text for bno085_sample.stability. */
const char *bno085_stability_str(uint8_t s);
