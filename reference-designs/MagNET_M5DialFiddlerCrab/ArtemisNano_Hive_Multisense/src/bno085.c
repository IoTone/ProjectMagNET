/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include "sh2.h"
#include "sh2_err.h"
#include "sh2_SensorValue.h"
#include "bno085.h"

#define RV_INTERVAL_US        100000	/* rotation vector, 10 Hz */
#define STABILITY_INTERVAL_US 1000000	/* stability classifier, 1 Hz */
#define POLL_MS               10

static const struct device *bus;
static uint16_t bus_addr;

static struct k_spinlock lock;
static struct bno085_sample latest;
static volatile bool need_config;
static K_SEM_DEFINE(started, 0, 1);
static int start_err = -ETIMEDOUT;

/* SHTP transfers are at most SH2_HAL_MAX_TRANSFER_IN; the IOM wants word-aligned buffers. */
static uint8_t rx_buf[SH2_HAL_MAX_TRANSFER_IN] __aligned(4);
static uint8_t tx_buf[SH2_HAL_MAX_TRANSFER_OUT] __aligned(4);

/* ---- SH-2 HAL over Zephyr I2C (polled) ---- */

static uint32_t hal_time_us(sh2_Hal_t *self)
{
	ARG_UNUSED(self);
	return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

static int hal_open(sh2_Hal_t *self)
{
	/* No RST line yet: SHTP soft reset (channel 1 "executable", command 1). */
	static const uint8_t soft_reset[] = {5, 0, 1, 0, 1};

	ARG_UNUSED(self);
	for (int tries = 0; tries < 5; tries++) {
		memcpy(tx_buf, soft_reset, sizeof(soft_reset));
		if (i2c_write(bus, tx_buf, sizeof(soft_reset), bus_addr) == 0) {
			k_msleep(300);
			return 0;
		}
		k_msleep(30);
	}
	return SH2_ERR_IO;
}

static void hal_close(sh2_Hal_t *self)
{
	ARG_UNUSED(self);
}

/*
 * Every read transaction starts with the 4-byte SHTP header, so read the header
 * to learn the length, then read the whole transfer (header included) again.
 */
static int hal_read(sh2_Hal_t *self, uint8_t *buf, unsigned len, uint32_t *t_us)
{
	uint8_t hdr[4] __aligned(4);
	uint16_t size;

	if (i2c_read(bus, hdr, sizeof(hdr), bus_addr) != 0) {
		return 0;
	}
	size = (hdr[0] | ((uint16_t)hdr[1] << 8)) & 0x7FFF;
	if (size == 0 || size > len || size > sizeof(rx_buf)) {
		return 0;
	}
	if (i2c_read(bus, rx_buf, size, bus_addr) != 0) {
		return 0;
	}
	memcpy(buf, rx_buf, size);
	*t_us = hal_time_us(self);
	return size;
}

static int hal_write(sh2_Hal_t *self, uint8_t *buf, unsigned len)
{
	ARG_UNUSED(self);
	if (len > sizeof(tx_buf)) {
		return 0;
	}
	memcpy(tx_buf, buf, len);
	return i2c_write(bus, tx_buf, len, bus_addr) == 0 ? (int)len : 0;
}

static sh2_Hal_t hal = {
	.open = hal_open,
	.close = hal_close,
	.read = hal_read,
	.write = hal_write,
	.getTimeUs = hal_time_us,
};

/* ---- SH-2 callbacks (run inside sh2_service on the IMU thread) ---- */

static void on_event(void *cookie, sh2_AsyncEvent_t *ev)
{
	ARG_UNUSED(cookie);
	if (ev->eventId == SH2_RESET) {
		k_spinlock_key_t key = k_spin_lock(&lock);

		latest.resets++;
		k_spin_unlock(&lock, key);
		need_config = true;
	}
}

static void on_sensor(void *cookie, sh2_SensorEvent_t *event)
{
	sh2_SensorValue_t v;

	ARG_UNUSED(cookie);
	if (sh2_decodeSensorEvent(&v, event) != SH2_OK) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	switch (v.sensorId) {
	case SH2_ROTATION_VECTOR: {
		float i = v.un.rotationVector.i, j = v.un.rotationVector.j;
		float k = v.un.rotationVector.k, r = v.un.rotationVector.real;
		const float deg = 57.2957795f;

		latest.qi = i;
		latest.qj = j;
		latest.qk = k;
		latest.qr = r;
		latest.accuracy_deg = v.un.rotationVector.accuracy * deg;
		latest.yaw = atan2f(2.0f * (i * j + k * r), i * i - j * j - k * k + r * r) * deg;
		latest.pitch = asinf(fmaxf(-1.0f, fminf(1.0f, -2.0f * (i * k - j * r)))) * deg;
		latest.roll = atan2f(2.0f * (j * k + i * r), -i * i - j * j + k * k + r * r) * deg;
		latest.reports++;
		latest.valid = true;
		break;
	}
	case SH2_STABILITY_CLASSIFIER:
		latest.stability = v.un.stabilityClassifier.classification;
		break;
	default:
		break;
	}
	k_spin_unlock(&lock, key);
}

static int enable_report(sh2_SensorId_t id, uint32_t interval_us)
{
	sh2_SensorConfig_t cfg = {.reportInterval_us = interval_us};

	return sh2_setSensorConfig(id, &cfg);
}

static void configure(void)
{
	int a = enable_report(SH2_ROTATION_VECTOR, RV_INTERVAL_US);
	int b = enable_report(SH2_STABILITY_CLASSIFIER, STABILITY_INTERVAL_US);

	if (a != SH2_OK || b != SH2_OK) {
		printk("bno085: enable reports failed (%d, %d)\n", a, b);
		return;
	}
	need_config = false;
}

static void imu_thread(void *p1, void *p2, void *p3)
{
	sh2_ProductIds_t ids = {0};
	int err;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	err = sh2_open(&hal, on_event, NULL);
	if (err != SH2_OK) {
		start_err = -ENODEV;
		k_sem_give(&started);
		return;
	}
	sh2_setSensorCallback(on_sensor, NULL);

	if (sh2_getProdIds(&ids) == SH2_OK && ids.numEntries > 0) {
		printk("bno085: part %u v%u.%u.%u build %u\n", ids.entry[0].swPartNumber,
		       ids.entry[0].swVersionMajor, ids.entry[0].swVersionMinor,
		       ids.entry[0].swVersionPatch, ids.entry[0].swBuildNumber);
	} else {
		printk("bno085: no product ID response\n");
	}
	configure();
	start_err = 0;
	k_sem_give(&started);

	for (;;) {
		sh2_service();
		if (need_config) {
			printk("bno085: hub reset, re-enabling reports\n");
			configure();
		}
		k_msleep(POLL_MS);
	}
}

K_THREAD_STACK_DEFINE(imu_stack, 3072);
static struct k_thread imu_tdata;

int bno085_start(const struct device *i2c, uint16_t addr)
{
	bus = i2c;
	bus_addr = addr;
	k_thread_create(&imu_tdata, imu_stack, K_THREAD_STACK_SIZEOF(imu_stack), imu_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&imu_tdata, "imu");
	k_sem_take(&started, K_SECONDS(5));
	return start_err;
}

void bno085_get(struct bno085_sample *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = latest;
	k_spin_unlock(&lock, key);
}

const char *bno085_stability_str(uint8_t s)
{
	static const char *const names[] = {"unknown", "on-table", "stationary", "stable",
					    "motion"};

	return s < ARRAY_SIZE(names) ? names[s] : "?";
}
