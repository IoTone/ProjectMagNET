/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include "opt4048.h"

#define REG_CH0       0x00	/* CH0..CH3: two 16-bit registers each, 0x00-0x07 */
#define REG_CONFIG    0x0A
#define REG_STATUS    0x0C
#define REG_DEVICE_ID 0x11

#define DEVICE_ID     0x0821

/* CONFIG: RANGE[13:10]=12 auto, CONVERSION_TIME[9:6]=8 (100 ms),
 * OPERATING_MODE[5:4]=3 continuous, LATCH[3]=1.  M5 moves to one-shot.
 */
#define CONFIG_CONTINUOUS ((12u << 10) | (8u << 6) | (3u << 4) | (1u << 3))

static int reg_read(const struct device *i2c, uint16_t addr, uint8_t reg, uint16_t *val)
{
	uint8_t b[2];

	if (i2c_burst_read(i2c, addr, reg, b, sizeof(b)) != 0) {
		return -EIO;
	}
	*val = ((uint16_t)b[0] << 8) | b[1];
	return 0;
}

static int reg_write(const struct device *i2c, uint16_t addr, uint8_t reg, uint16_t val)
{
	uint8_t b[3] = {reg, val >> 8, val & 0xFF};

	return i2c_write(i2c, b, sizeof(b), addr) ? -EIO : 0;
}

static uint8_t parity(uint32_t v)
{
	return __builtin_parity(v);
}

/* Datasheet CRC over exponent E, 20-bit mantissa R and counter C. */
static uint8_t channel_crc(uint8_t e, uint32_t r, uint8_t c)
{
	uint8_t x0 = parity(e) ^ parity(r) ^ parity(c);
	uint8_t x1 = parity(c & 0xA) ^ parity(r & 0xAAAAA) ^ parity(e & 0xA);
	uint8_t x2 = parity(c & 0x8) ^ parity(r & 0x88888) ^ parity(e & 0x8);
	uint8_t x3 = parity(r & 0x80808);

	return x0 | (x1 << 1) | (x2 << 2) | (x3 << 3);
}

int opt4048_init(const struct device *i2c, uint16_t addr)
{
	uint16_t id;

	if (reg_read(i2c, addr, REG_DEVICE_ID, &id) != 0) {
		return -ENODEV;
	}
	printk("opt4048: ID=0x%04x\n", id);
	if (id != DEVICE_ID) {
		return -ENOTSUP;
	}
	return reg_write(i2c, addr, REG_CONFIG, CONFIG_CONTINUOUS);
}

int opt4048_read(const struct device *i2c, uint16_t addr, struct opt4048_sample *s)
{
	/* Datasheet matrix: [CH0 CH1 CH2] x M = [X Y Z], lux = CH1 * 2.15e-3. */
	static const float m[3][3] = {
		{2.34892992e-04f, -1.89652390e-05f, 1.20811684e-05f},
		{4.07467441e-05f, 1.98958202e-04f, -1.58848115e-05f},
		{9.28619404e-05f, -1.69739553e-05f, 6.74021520e-04f},
	};
	uint8_t b[16];
	float xyz[3] = {0};

	if (i2c_burst_read(i2c, addr, REG_CH0, b, sizeof(b)) != 0) {
		return -EIO;
	}

	for (int ch = 0; ch < 4; ch++) {
		const uint8_t *p = &b[4 * ch];
		uint8_t e = p[0] >> 4;
		uint32_t r = ((uint32_t)(p[0] & 0x0F) << 16) | ((uint32_t)p[1] << 8) | p[2];
		uint8_t c = p[3] >> 4;

		if ((p[3] & 0x0F) != channel_crc(e, r, c)) {
			return -EBADMSG;
		}
		s->raw[ch] = r << e;
	}
	if (s->raw[0] == 0 && s->raw[1] == 0 && s->raw[2] == 0 && s->raw[3] == 0) {
		return -EAGAIN;
	}

	for (int i = 0; i < 3; i++) {
		for (int ch = 0; ch < 3; ch++) {
			xyz[i] += (float)s->raw[ch] * m[ch][i];
		}
	}
	s->lux = (float)s->raw[1] * 2.15e-3f;

	float sum = xyz[0] + xyz[1] + xyz[2];

	if (sum <= 0.0f) {
		s->cie_x = s->cie_y = s->cct = 0.0f;
		return 0;
	}
	s->cie_x = xyz[0] / sum;
	s->cie_y = xyz[1] / sum;

	/* McCamy's approximation, valid roughly 2000-12500 K. */
	float n = (s->cie_x - 0.3320f) / (0.1858f - s->cie_y);

	s->cct = 437.0f * n * n * n + 3601.0f * n * n + 6861.0f * n + 5517.0f;
	if (s->cct < 0.0f) {
		s->cct = 0.0f;
	}
	return 0;
}
