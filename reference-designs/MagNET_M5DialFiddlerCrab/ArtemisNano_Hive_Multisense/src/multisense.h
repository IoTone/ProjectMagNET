/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

/*
 * Shared state between the sensor loop (main.c) and the radio (hive_ble.c).
 * main.c owns the sensors and publishes a snapshot every report period.
 */

/* Advert sensor bitmap (README 5.1). Set from the boot probe, never hard-coded. */
#define MS_CAP_PRESENCE    BIT(0)
#define MS_CAP_LUX         BIT(1)
#define MS_CAP_COLOR       BIT(2)
#define MS_CAP_TIME        BIT(3)
#define MS_CAP_BATTERY     BIT(4)
#define MS_CAP_SOUND       BIT(5)
#define MS_CAP_ORIENTATION BIT(6)

#define MS_ROLE_MULTISENSOR 15
#define MS_FW_VERSION       "0.4.1-m4"

/* Battery temperature sentinel until a thermistor is fitted (deci-degrees C). */
#define TBAT_NONE_DC INT16_MIN

struct ms_readings {
	uint16_t caps;		/* MS_CAP_* detected at boot */
	uint8_t seq;		/* increments with each snapshot */

	/* battery (MS_CAP_BATTERY) */
	uint16_t vbat_mv;
	uint8_t soc_pct;	/* 0xFF = unknown */
	int16_t tbat_dc;	/* TBAT_NONE_DC = no thermistor */

	/* light (MS_CAP_LUX / MS_CAP_COLOR) */
	bool light_ok;
	float lux;
	float cie_x, cie_y;
	float cct;

	/* IMU (MS_CAP_ORIENTATION) */
	bool imu_ok;
	float yaw, pitch, roll;
	float heading_acc_deg;
	uint8_t stability;	/* 0 unknown, 1 on table, 2 stationary, 3 stable, 4 motion */
	uint32_t sig_motion;

	/* time (MS_CAP_TIME) */
	bool time_ok;
	char time_iso[32];	/* YYYY-MM-DDTHH:MM:SS (sized for the compiler's worst case) */
};

/* Copy the latest snapshot (thread-safe). */
void ms_readings_get(struct ms_readings *out);

/* Stable 4-byte node ID from the Apollo3 chip ID. */
const uint8_t *ms_device_id(void);
