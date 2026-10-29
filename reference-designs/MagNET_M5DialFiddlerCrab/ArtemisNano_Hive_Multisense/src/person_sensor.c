/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include "person_sensor.h"

/* Register map (write only, [reg, value]); defaults from the developer guide. */
#define REG_MODE        0x01	/* 0 standby, 1 continuous (default 1) */
#define REG_ENABLE_ID   0x02	/* 0 boxes only (default), 1 run recognition */
#define REG_SINGLE_SHOT 0x03	/* trigger; value undocumented, see the header */
#define REG_LABEL_ID    0x04	/* enrol the next face as ID N, 0-7 */
#define REG_PERSIST_IDS 0x05	/* keep IDs across power loss (default 1) */
#define REG_ERASE_IDS   0x06
#define REG_DEBUG_MODE  0x07	/* LED indicator (default 1) */

#define MODE_STANDBY    0x00
#define MODE_CONTINUOUS 0x01

/* One inference, per the developer guide's latency figure. */
#define SINGLE_SHOT_MS 200

/*
 * Packet layout, offsets into the 39-byte read:
 *   0-1   reserved
 *   2-3   data_size
 *   4     num_faces
 *   5+8i  face i: confidence, left, top, right, bottom, id_confidence, id,
 *         is_facing
 *   37-38 CRC16
 *
 * Two documented ambiguities, both benign for a presence count and both worth
 * resolving on the bench before anyone uses the boxes for geometry:
 *   - bytes 8/9 are "right"/"bottom" in the data-format table and in ESPHome's
 *     member names, but "width"/"height" in the guide's own C struct.
 *   - the CRC16 polynomial, initial value and byte order are not documented
 *     anywhere public; the vendor's person_sensor_pico_c driver is the
 *     authoritative source. ESPHome does not check it either, so neither do we
 *     -- a wrong check is worse than none, since it would reject good packets.
 *     The plausibility check below is what guards against garbage.
 */
#define OFF_NUM_FACES 4
#define OFF_FACE0     5
#define FACE_STRIDE   8

static int read_packet(const struct device *i2c, uint16_t addr, uint8_t *buf)
{
	/*
	 * A PLAIN read -- no register pointer. i2c_read(), never i2c_burst_read():
	 * the latter writes a pointer byte first and this device does not expect
	 * one. Getting this wrong is the classic "healthy board reads as dead".
	 */
	return i2c_read(i2c, buf, PERSON_SENSOR_PACKET_LEN, addr) ? -EIO : 0;
}

static int parse(const uint8_t *buf, struct person_sensor_sample *s)
{
	const uint8_t n = buf[OFF_NUM_FACES];

	/*
	 * The only field worth validating. data_size is not checked: ESPHome's
	 * comment says it "should be 40" while the documented layout implies 33,
	 * so no value for it can be trusted as a gate.
	 */
	if (n > PERSON_SENSOR_MAX_FACES) {
		return -EBADMSG;
	}

	memset(s, 0, sizeof(*s));
	s->num_faces = n;
	for (uint8_t i = 0; i < n; i++) {
		const uint8_t *p = &buf[OFF_FACE0 + (FACE_STRIDE * i)];

		s->face[i].confidence = p[0];
		s->face[i].left = p[1];
		s->face[i].top = p[2];
		s->face[i].right = p[3];
		s->face[i].bottom = p[4];
		s->face[i].id_confidence = (int8_t)p[5];
		s->face[i].id = (int8_t)p[6];
		s->face[i].is_facing = p[7] != 0;
	}
	return 0;
}

int person_sensor_init(const struct device *i2c, uint16_t addr)
{
	uint8_t buf[PERSON_SENSOR_PACKET_LEN];
	struct person_sensor_sample s;

	if (read_packet(i2c, addr, buf) != 0) {
		return -ENODEV;
	}
	if (parse(buf, &s) != 0) {
		printk("person: answered at 0x%02x but num_faces=%u\n", addr,
		       buf[OFF_NUM_FACES]);
		return -EBADMSG;
	}
	printk("person: ok (%u face(s) in frame at probe)\n", s.num_faces);
	return 0;
}

int person_sensor_read(const struct device *i2c, uint16_t addr,
		       struct person_sensor_sample *s)
{
	uint8_t buf[PERSON_SENSOR_PACKET_LEN];
	int err = read_packet(i2c, addr, buf);

	if (err) {
		return err;
	}
	return parse(buf, s);
}

const struct person_sensor_face *person_sensor_best(
	const struct person_sensor_sample *s)
{
	const struct person_sensor_face *best = NULL;

	for (uint8_t i = 0; i < s->num_faces; i++) {
		if (!best || s->face[i].confidence > best->confidence) {
			best = &s->face[i];
		}
	}
	return best;
}

static int reg_write(const struct device *i2c, uint16_t addr, uint8_t reg, uint8_t val)
{
	uint8_t b[2] = {reg, val};

	return i2c_write(i2c, b, sizeof(b), addr) ? -EIO : 0;
}

int person_sensor_standby(const struct device *i2c, uint16_t addr)
{
	return reg_write(i2c, addr, REG_MODE, MODE_STANDBY);
}

int person_sensor_continuous(const struct device *i2c, uint16_t addr)
{
	return reg_write(i2c, addr, REG_MODE, MODE_CONTINUOUS);
}

int person_sensor_trigger(const struct device *i2c, uint16_t addr)
{
	int err = reg_write(i2c, addr, REG_SINGLE_SHOT, 0x01);

	if (err) {
		return err;
	}
	k_msleep(SINGLE_SHOT_MS);
	return 0;
}
