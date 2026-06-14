#ifndef CRAW_IMU_MPU6886_H
#define CRAW_IMU_MPU6886_H
#define CRAW_IMU_MPU6886_VERSION "0.1.0"

/* craw_imu_mpu6886 — MPU6886 6-DoF IMU sampler for the M5StickC Plus.
 *
 * The StickC Plus carries an InvenSense MPU6886 (accel + gyro, no
 * magnetometer) at I2C address 0x68 on the SAME internal bus as the AXP192
 * PMIC (0x34), pins SDA=G21 / SCL=G22. That bus is owned and initialised by
 * M5GFX (port 1) for backlight/power control, so this driver does NOT bring
 * up its own I2C controller — it reuses M5GFX's lgfx::i2c helpers (the same
 * path the battery monitor uses). Consequence: craw_imu_mpu6886_init() MUST be
 * called AFTER display.init().
 *
 * Orientation: roll/pitch are derived directly from the accelerometer
 * (gravity vector), so they're absolute and drift-free. Yaw is integrated
 * from the gyro Z axis and drifts (no magnetometer) — call _calibrate() while
 * flat and at rest to zero the gyro bias, which keeps yaw stable for minutes.
 *
 * Frame (StickC Plus flat on a table, screen up, USB-C down):
 *   Z up (accel ~ +9.81 m/s² on Z at rest), X/Y in the screen plane.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRAW_IMU_MPU6886_ADDR      0x68   /* AD0 low on StickC Plus */
#define CRAW_IMU_MPU6886_WHOAMI    0x19   /* WHO_AM_I value for MPU6886 */
#define CRAW_IMU_MPU6886_I2C_PORT  1      /* M5GFX internal bus (AXP192 + IMU) */
#define CRAW_IMU_MPU6886_DEFAULT_HZ 50

typedef struct {
    int     i2c_port;     /* lgfx i2c port; 0 → CRAW_IMU_MPU6886_I2C_PORT */
    uint8_t addr;         /* I2C address; 0 → CRAW_IMU_MPU6886_ADDR */
    int     sample_hz;    /* sample loop rate; 0 → CRAW_IMU_MPU6886_DEFAULT_HZ */
} craw_imu_mpu6886_config_t;

/* All SI units, sensor body frame. Calibration offsets already applied. */
typedef struct {
    float    roll_rad;    /* from accel — absolute */
    float    pitch_rad;   /* from accel — absolute */
    float    yaw_rad;     /* gyro-integrated, post-calibration — drifts */
    float    gyro_x, gyro_y, gyro_z;     /* rad/s */
    float    accel_x, accel_y, accel_z;  /* m/s² (gravity included) */
    float    temp_c;                     /* die temperature */
    uint64_t timestamp_us;               /* esp_timer_get_time() */
} craw_imu_mpu6886_snapshot_t;

typedef struct {
    bool     initialized;
    bool     running;
    bool     calibrated;
    uint64_t samples;
    int      sample_hz;
    float    gyro_bias_x, gyro_bias_y, gyro_bias_z;   /* rad/s */
    float    accel_bias_x, accel_bias_y, accel_bias_z;/* m/s² */
} craw_imu_mpu6886_stats_t;

/* Probe WHO_AM_I, reset + configure the sensor (±8g / ±2000 dps). Loads any
 * persisted calibration from NVS. Idempotent. Returns ESP_ERR_NOT_FOUND if
 * the chip doesn't answer / WHO_AM_I mismatches (e.g. SH200Q variant). */
esp_err_t craw_imu_mpu6886_init(const craw_imu_mpu6886_config_t *cfg);

/* Start / stop the background sample task. Idempotent. */
esp_err_t craw_imu_mpu6886_start(void);
esp_err_t craw_imu_mpu6886_stop(void);
bool      craw_imu_mpu6886_is_running(void);

/* Flat-on-table calibration: averages `samples` readings (caller must keep the
 * device still and level). Computes gyro bias (expects ~0 at rest) and accel
 * X/Y bias (expects ~0) — Z is left intact so gravity still reads +1g. Stores
 * the result to NVS. Blocks ~ samples/sample_hz seconds. Returns ESP_OK.
 * Pass samples<=0 for a sensible default (~2 s worth). */
esp_err_t craw_imu_mpu6886_calibrate(int samples);

bool      craw_imu_mpu6886_is_calibrated(void);

/* Erase persisted calibration + reset biases to zero (forces re-cal). */
void      craw_imu_mpu6886_clear_cal(void);

/* Atomic snapshot of the latest sample. */
void      craw_imu_mpu6886_snapshot(craw_imu_mpu6886_snapshot_t *out);

/* Reset the yaw datum to the current heading. */
void      craw_imu_mpu6886_zero_yaw(void);

void      craw_imu_mpu6886_stats(craw_imu_mpu6886_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif
