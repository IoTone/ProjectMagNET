/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

/*
 * Useful Sensors Person Sensor (SparkFun SEN21231) at I2C 0x62.
 *
 * On-module face detection, and optional recognition of up to 8 enrolled IDs.
 * No image ever leaves the board and the firmware is not updatable, which is
 * the whole privacy argument for using it in a hive.
 *
 * v1.0, v1.1 and v2.0 are software-compatible: same address, same packet, same
 * registers. v1.1's changes are mechanical (snap-off panel carrying the
 * mounting holes and bare signal pads, camera module glued rather than
 * press-fit). v2.0 swapped the processor and image sensor and detects faster.
 *
 * TWO THINGS THAT MAKE A NEW DRIVER LOOK DEAD ON HEALTHY HARDWARE:
 *
 *  1. Results are a PLAIN READ of 39 bytes with NO register pointer. A
 *     register-style read -- write the pointer, repeated start, read -- does
 *     not work. Writes, by contrast, are register-style [reg, value].
 *  2. 3.3 V only. The Qwiic rail is 3.3 V so the chain is fine, but it will
 *     not tolerate 5 V.
 *
 * POWER: about 150 mW, roughly 45 mA at 3.3 V. That is ~1800x the OPT4048's
 * 24 uA and dwarfs even the red power LED that README 4.2 calls the dominant
 * sleep load. Left in its default continuous mode it sets the floor for the
 * whole node and makes both M5 power architectures moot. Battery operation
 * means standby plus single-shot -- see person_sensor_standby() and the
 * warning on person_sensor_trigger().
 */

#define PERSON_SENSOR_ADDR       0x62
#define PERSON_SENSOR_MAX_FACES  4
#define PERSON_SENSOR_PACKET_LEN 39

struct person_sensor_face {
	uint8_t confidence;	/* box confidence; 0-100 per the dev guide */
	uint8_t left, top;
	uint8_t right, bottom;	/* the sources disagree: may be width/height (see .c) */
	int8_t id_confidence;	/* <= 0 when the face is not recognised */
	int8_t id;		/* 0-7 when recognised */
	bool is_facing;
};

struct person_sensor_sample {
	uint8_t num_faces;	/* 0-4 */
	struct person_sensor_face face[PERSON_SENSOR_MAX_FACES];
};

/*
 * Probe the sensor. Deliberately writes no registers: the board boots in
 * continuous mode with its debug LED enabled, which is what bring-up wants,
 * and every register value this driver could write at init is either
 * unnecessary or (for single-shot) unverified.
 *
 * Returns 0, -ENODEV (no answer) or -EBADMSG (answered, but not with a packet
 * this driver recognises).
 */
int person_sensor_init(const struct device *i2c, uint16_t addr);

/* Returns 0, -EIO (bus) or -EBADMSG (implausible packet). */
int person_sensor_read(const struct device *i2c, uint16_t addr,
		       struct person_sensor_sample *s);

/* Highest-confidence face, or NULL when nobody is in frame. */
const struct person_sensor_face *person_sensor_best(
	const struct person_sensor_sample *s);

/* Standby: stops capturing, the low-power state. Needed before a trigger. */
int person_sensor_standby(const struct device *i2c, uint16_t addr);

/* Continuous: about 7 Hz, or 5 Hz with recognition enabled. */
int person_sensor_continuous(const struct device *i2c, uint16_t addr);

/*
 * Fire one inference from standby, then wait ~200 ms before reading.
 *
 * UNVERIFIED: the developer guide documents register 0x03 as "trigger a
 * single-shot inference, only works in standby mode" but never states the
 * value to write. This sends 0x01. Confirm on the bench before relying on the
 * low-power path -- if the value is wrong the sensor stays in standby and
 * reads come back as a stale or empty packet, which looks exactly like a dead
 * board.
 */
int person_sensor_trigger(const struct device *i2c, uint16_t addr);
