#ifndef CRAW_IMU_H
#define CRAW_IMU_H

/* craw_imu — BMI270 6-DoF IMU sampler + Madgwick AHRS for the M5Capsule.
 *
 * The Capsule carries a Bosch BMI270 (accel + gyro, no magnetometer) on
 * I2C bus G8/G40. This component:
 *
 *   - Owns the I2C master bus (callers shouldn't init it separately;
 *     they can call craw_imu_get_i2c_bus() to share it with peers like
 *     the BM8563 RTC).
 *   - Initializes the BMI270 via the espressif/bmi270 managed component.
 *   - Runs a sample task at sample_hz Hz that reads accel + gyro and
 *     updates a Madgwick AHRS filter to produce an orientation quat.
 *   - Exposes the latest fused orientation as roll/pitch/yaw plus the
 *     raw angular_velocity + acceleration vectors for downstream
 *     visualisation (UC4's airplane attitude cell).
 *
 * Threading: the sampler runs in its own task; readers grab a snapshot
 * via craw_imu_snapshot() which is portMUX-guarded so the reader sees a
 * consistent set of fields. The HTTP handler (craw_imu_http) and the
 * Forth `imu-status` word both use that path.
 *
 * Magnetometer caveat: there isn't one. Yaw is integrated from the
 * gyro's Z-axis (via Madgwick's 6-DoF mode) and drifts over time at
 * roughly 1-2°/min once thermally stable. `craw_imu_zero()` resets
 * the heading datum — the visible UC4 heading is `yaw - yaw0`.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BMI270 on the M5Capsule sits at the H-address (0x69) — SDO is
 * pulled HIGH on this hardware revision. The L-address (0x68) is
 * what Bosch's datasheet calls the "default," and what most product
 * pages list, but the Capsule wires it the other way. `imu-scan`
 * confirmed: 0x51 (BM8563 RTC) + 0x69 (BMI270) on G8/G10. */
#define CRAW_IMU_DEFAULT_ADDR    0x69

/* I2C pins for the Capsule's internal sensor bus.
 *
 * Authoritative source: M5Unified's `_pin_table_i2c_ex_in` for
 * `board_M5Capsule` (SCL_in=10, SDA_in=8). An earlier comment in
 * `src/main.c` claimed SCL=40, but GPIO 40 is actually the PDM mic
 * WS line — bringing up I2C on G8/G40 looks fine at the bus level
 * (no GPIO config error, internal pull-ups engage) but every
 * transaction NACKs because the BMI270 isn't on that bus. */
#define CRAW_IMU_DEFAULT_SDA     8
#define CRAW_IMU_DEFAULT_SCL     10

/* 50 Hz is the sweet spot: high enough that the user's head motion in
 * UC4 reads as smooth attitude updates, low enough that Madgwick has
 * adequate time per step and the I2C bus isn't saturated. The cell
 * polls at 5 Hz (every 200 ms) so the sampler is always ahead. */
#define CRAW_IMU_DEFAULT_HZ      50

typedef struct {
    int   sda;            /* GPIO for SDA — default CRAW_IMU_DEFAULT_SDA */
    int   scl;            /* GPIO for SCL — default CRAW_IMU_DEFAULT_SCL */
    uint8_t addr;         /* BMI270 I2C address — default 0x68 */
    int   sample_hz;      /* Sample loop frequency — default 50 */
    float beta;           /* Madgwick gain. 0.1 = reasonable default;
                           * higher = trusts accel more (faster
                           * settling but more jitter); lower = trusts
                           * gyro more (smoother but slower correction
                           * for the accel ground truth). */
} craw_imu_config_t;

/* Snapshot of the latest sample + fusion result. All fields are SI
 * units in the sensor body frame:
 *   - Z up (accel reads ~9.81 m/s² on Z at rest, panel-up),
 *   - X forward (out of the USB-C port),
 *   - Y right.
 * The mapping to the UC4 airplane's body frame is done client-side in
 * liveImuCell.ts (YXZ Euler with the pitch/yaw/roll order the cell
 * already uses). */
typedef struct {
    /* Fused orientation. yaw_rad is post-zero (subtracted by the
     * datum set on the last craw_imu_zero call). */
    float    roll_rad;
    float    pitch_rad;
    float    yaw_rad;
    /* Raw angular velocity (gyro, rad/s) and linear acceleration
     * (accel, m/s² — gravity included, not stripped). Surfaced raw so
     * the UC4 cell could later add a velocity-derived bump-cue without
     * a firmware roundtrip. */
    float    gyro_x, gyro_y, gyro_z;
    float    accel_x, accel_y, accel_z;
    /* Microseconds since boot (esp_timer_get_time). Matches the
     * timestamp_us field the mock-join-server emits, so the UC4 cell
     * doesn't need a per-device adapter. */
    uint64_t timestamp_us;
} craw_imu_snapshot_t;

typedef struct {
    bool      initialized;
    bool      running;
    uint64_t  samples;          /* total Madgwick updates since start */
    uint64_t  last_sample_us;
    int       sample_hz;
    float     yaw_zero_rad;     /* current datum (subtracted from raw yaw) */
} craw_imu_stats_t;

/* One-time init: brings up I2C, creates the BMI270 device, uploads
 * the config blob (Bosch firmware), configures ODR/range, but does NOT
 * start the sample task. Idempotent — second call returns ESP_OK
 * without reinitialising. */
esp_err_t craw_imu_init(const craw_imu_config_t *cfg);

/* Returns the shared I2C master bus handle once craw_imu_init has run,
 * or NULL otherwise. Lets other I2C peripherals (BM8563 RTC, future
 * peripherals on the same bus) attach without re-initialising. */
i2c_master_bus_handle_t craw_imu_get_i2c_bus(void);

/* Start / stop the sampler task. Idempotent. */
esp_err_t craw_imu_start(void);
esp_err_t craw_imu_stop(void);
bool      craw_imu_is_running(void);

/* Atomic snapshot of the latest sample. Out param must be non-NULL.
 * Returns the timestamp_us field too so the caller can detect
 * stale-snapshot conditions cheaply. */
void craw_imu_snapshot(craw_imu_snapshot_t *out);

/* Set the current yaw as the heading 0 datum. Subsequent snapshots
 * return yaw_rad = (raw_yaw - datum), wrapped into (-π, π]. */
void craw_imu_zero(void);

/* Diagnostics for the Forth `imu-status` word. */
void craw_imu_stats(craw_imu_stats_t *out);

/* I2C bus probe — the firmware equivalent of Linux's `i2cdetect`.
 * Walks every 7-bit address 0x03..0x77 on the bus formed by the
 * given pins, calling i2c_master_probe() on each. Prints a 16×8
 * grid: address responding = its hex, no ACK = "--". Useful when
 * `imu-on` can't reach the BMI270 — distinguishes "wrong pin map"
 * (entire bus silent) from "wrong address" (chip answers at a
 * different address) from "dead chip" (correct pins, no ACK
 * anywhere on the bus).
 *
 * Tries to reuse the existing bus if it's open with matching pins;
 * otherwise spins up a temporary I2C_NUM_1 bus, scans, tears down.
 * Idempotent — leaves no state behind. */
esp_err_t craw_imu_bus_scan(int sda, int scl);

/* HTTP server lifecycle. Brings up esp_http_server on :80 with one
 * handler: GET /api/v1/sensor/imu (plus OPTIONS preflight). Paired
 * with the Forth `imu-on`/`imu-off` words. */
esp_err_t craw_imu_http_start(void);
esp_err_t craw_imu_http_stop(void);
bool      craw_imu_http_running(void);

#ifdef __cplusplus
}
#endif
#endif
