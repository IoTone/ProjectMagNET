/* craw_imu — BMI270 sampler + Madgwick AHRS for the M5Capsule.
 *
 * See craw_imu.h for the public contract and the design rationale
 * (why 6-DoF only, why caller-shared I2C bus, why post-zero yaw).
 *
 * The Madgwick implementation here is the standard 6-DoF (IMU-only)
 * variant from Madgwick's 2010 paper, retyped from scratch in single
 * precision. Beta gain ~0.1 gives a roll/pitch convergence time around
 * 1 s at startup and tracks dynamic motion without obvious lag. The
 * fast-inverse-sqrt trick from the original implementation isn't worth
 * the readability cost on a modern S3 — plain 1.0f / sqrtf() is fine.
 */

#include "craw_imu.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "bmi270.h"

static const char *TAG = "craw_imu";

#define I2C_PORT        I2C_NUM_0
#define I2C_FREQ_HZ     400000
#define G_TO_MPS2       9.80665f
#define DEG_TO_RAD      0.01745329251994f

/* --------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------*/

static i2c_master_bus_handle_t s_bus     = NULL;
static bmi270_handle_t        *s_bmi     = NULL;
static TaskHandle_t            s_task    = NULL;
static portMUX_TYPE            s_lock    = portMUX_INITIALIZER_UNLOCKED;

static craw_imu_config_t       s_cfg = {
    .sda = CRAW_IMU_DEFAULT_SDA,
    .scl = CRAW_IMU_DEFAULT_SCL,
    .addr = CRAW_IMU_DEFAULT_ADDR,
    .sample_hz = CRAW_IMU_DEFAULT_HZ,
    .beta = 0.1f,
};

/* Madgwick state: orientation quaternion. Initialised to identity. */
static float s_q0 = 1.0f, s_q1 = 0.0f, s_q2 = 0.0f, s_q3 = 0.0f;

/* Latest reading mirror — populated by the sample task, read by
 * craw_imu_snapshot via the portMUX. We keep the raw values + the
 * derived euler angles together so a reader gets a self-consistent
 * snapshot (otherwise gyro could be stamped at t=k but euler at t=k-1
 * if the task preempted between writes). */
static craw_imu_snapshot_t s_latest = {0};
static float               s_yaw_zero_rad = 0.0f;

static bool     s_initialized = false;
static bool     s_running     = false;
static uint64_t s_samples     = 0;

/* --------------------------------------------------------------------
 * Madgwick 6-DoF AHRS update
 * --------------------------------------------------------------------*/

static void madgwick_update(float gx, float gy, float gz,
                            float ax, float ay, float az,
                            float dt)
{
    float q0 = s_q0, q1 = s_q1, q2 = s_q2, q3 = s_q3;
    const float beta = s_cfg.beta;

    /* Rate of change of quaternion from gyroscope */
    float qDot1 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
    float qDot2 = 0.5f * ( q0 * gx + q2 * gz - q3 * gy);
    float qDot3 = 0.5f * ( q0 * gy - q1 * gz + q3 * gx);
    float qDot4 = 0.5f * ( q0 * gz + q1 * gy - q2 * gx);

    /* If accel is non-zero, fold in the gravity-direction correction. */
    float aSq = ax * ax + ay * ay + az * az;
    if (aSq > 1e-6f) {
        const float recipNorm = 1.0f / sqrtf(aSq);
        ax *= recipNorm; ay *= recipNorm; az *= recipNorm;

        /* Gradient descent step (Madgwick eq. 25 — IMU-only form). */
        const float _2q0 = 2.0f * q0;
        const float _2q1 = 2.0f * q1;
        const float _2q2 = 2.0f * q2;
        const float _2q3 = 2.0f * q3;
        const float _4q0 = 4.0f * q0;
        const float _4q1 = 4.0f * q1;
        const float _4q2 = 4.0f * q2;
        const float _8q1 = 8.0f * q1;
        const float _8q2 = 8.0f * q2;
        const float q0q0 = q0 * q0;
        const float q1q1 = q1 * q1;
        const float q2q2 = q2 * q2;
        const float q3q3 = q3 * q3;

        float s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
        float s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
        float s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
        float s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;

        float sNorm = sqrtf(s0*s0 + s1*s1 + s2*s2 + s3*s3);
        if (sNorm > 1e-9f) {
            const float sRecip = 1.0f / sNorm;
            s0 *= sRecip; s1 *= sRecip; s2 *= sRecip; s3 *= sRecip;
            qDot1 -= beta * s0;
            qDot2 -= beta * s1;
            qDot3 -= beta * s2;
            qDot4 -= beta * s3;
        }
    }

    /* Integrate. */
    q0 += qDot1 * dt;
    q1 += qDot2 * dt;
    q2 += qDot3 * dt;
    q3 += qDot4 * dt;

    /* Normalise. */
    float qNorm = sqrtf(q0*q0 + q1*q1 + q2*q2 + q3*q3);
    if (qNorm > 1e-9f) {
        const float qRecip = 1.0f / qNorm;
        s_q0 = q0 * qRecip;
        s_q1 = q1 * qRecip;
        s_q2 = q2 * qRecip;
        s_q3 = q3 * qRecip;
    }
}

/* Convert the active quaternion to roll/pitch/yaw (ZYX intrinsic Tait-
 * Bryan order — what pilots think of as "bank/pitch/heading"). */
static void quat_to_euler(float q0, float q1, float q2, float q3,
                          float *roll, float *pitch, float *yaw)
{
    /* Standard formulas — kept here verbatim so a future maintainer can
     * cross-check against a reference (Wikipedia "Conversion between
     * quaternions and Euler angles"). */
    *roll  = atan2f(2.0f * (q0 * q1 + q2 * q3),
                    1.0f - 2.0f * (q1 * q1 + q2 * q2));
    float sinp = 2.0f * (q0 * q2 - q3 * q1);
    if (sinp >  1.0f) sinp =  1.0f;
    if (sinp < -1.0f) sinp = -1.0f;
    *pitch = asinf(sinp);
    *yaw   = atan2f(2.0f * (q0 * q3 + q1 * q2),
                    1.0f - 2.0f * (q2 * q2 + q3 * q3));
}

/* Wrap an angle into (-π, π]. Used after subtracting the zero datum
 * from the raw yaw so the heading display doesn't jump from -π to π
 * across the wrap. */
static float wrap_pi(float a)
{
    const float TWO_PI = 6.28318530717958f;
    while (a >   3.14159265358979f) a -= TWO_PI;
    while (a <= -3.14159265358979f) a += TWO_PI;
    return a;
}

/* --------------------------------------------------------------------
 * Sample task
 * --------------------------------------------------------------------*/

static void sample_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / s_cfg.sample_hz);
    const float dt = 1.0f / (float)s_cfg.sample_hz;
    TickType_t last_wake = xTaskGetTickCount();
    uint64_t prev_us = 0;

    ESP_LOGI(TAG, "sample task started @%d Hz, beta=%.2f",
             s_cfg.sample_hz, s_cfg.beta);

    while (s_running) {
        float ax_g, ay_g, az_g;
        float gx_dps, gy_dps, gz_dps;

        esp_err_t ar = bmi270_get_acce_data(s_bmi, &ax_g, &ay_g, &az_g);
        esp_err_t gr = bmi270_get_gyro_data(s_bmi, &gx_dps, &gy_dps, &gz_dps);

        if (ar == ESP_OK && gr == ESP_OK) {
            /* Unit conversions. BMI270 yields g and dps; Madgwick takes
             * rad/s for the gyro and any consistent unit for accel
             * (it normalises internally). We feed m/s² for accel
             * because it's what the UC4 mark expects raw. */
            const float ax = ax_g * G_TO_MPS2;
            const float ay = ay_g * G_TO_MPS2;
            const float az = az_g * G_TO_MPS2;
            const float gx = gx_dps * DEG_TO_RAD;
            const float gy = gy_dps * DEG_TO_RAD;
            const float gz = gz_dps * DEG_TO_RAD;

            /* Use measured dt when available — drift correction lives
             * or dies on dt accuracy under FreeRTOS scheduling jitter. */
            const uint64_t now_us = (uint64_t)esp_timer_get_time();
            float dt_meas = dt;
            if (prev_us != 0) {
                const float d = (float)((now_us - prev_us) * 1e-6);
                if (d > 0.001f && d < 0.5f) dt_meas = d;
            }
            prev_us = now_us;

            madgwick_update(gx, gy, gz, ax, ay, az, dt_meas);

            float roll, pitch, yaw_raw;
            quat_to_euler(s_q0, s_q1, s_q2, s_q3, &roll, &pitch, &yaw_raw);
            const float yaw_disp = wrap_pi(yaw_raw - s_yaw_zero_rad);

            /* Mirror into the published snapshot under the lock. */
            portENTER_CRITICAL(&s_lock);
            s_latest.roll_rad     = roll;
            s_latest.pitch_rad    = pitch;
            s_latest.yaw_rad      = yaw_disp;
            s_latest.gyro_x       = gx;
            s_latest.gyro_y       = gy;
            s_latest.gyro_z       = gz;
            s_latest.accel_x      = ax;
            s_latest.accel_y      = ay;
            s_latest.accel_z      = az;
            s_latest.timestamp_us = now_us;
            s_samples++;
            portEXIT_CRITICAL(&s_lock);
        } else {
            ESP_LOGW(TAG, "bmi270 read: accel rc=%d gyro rc=%d", ar, gr);
        }

        vTaskDelayUntil(&last_wake, period);
    }

    ESP_LOGI(TAG, "sample task exiting");
    s_task = NULL;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------*/

/* M5Capsule power-latch GPIO. Driving this HIGH at boot keeps the
 * 3V3 rail alive after the user releases the physical power button.
 * On USB-C power the rail survives without it (USB feeds 3V3 through
 * the LDO directly), so the firmware boots and runs — but on battery,
 * the rail collapses at button-release if HOLD is left floating, and
 * every subsequent sensor read on the internal bus NACKs.
 *
 * Authoritative source: M5Unified's `_pin_table_other1` for
 * `board_M5Capsule` lists GPIO 46 as the power-latch pin. */
#define CRAW_IMU_HOLD_GPIO       46

static void capsule_power_hold(void)
{
    static bool held = false;
    if (held) return;
    gpio_config_t hold = {
        .pin_bit_mask = (1ULL << CRAW_IMU_HOLD_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&hold);
    gpio_set_level(CRAW_IMU_HOLD_GPIO, 1);
    held = true;
    ESP_LOGI(TAG, "HOLD asserted on GPIO %d (battery rail latched)",
             CRAW_IMU_HOLD_GPIO);
}

esp_err_t craw_imu_init(const craw_imu_config_t *cfg)
{
    if (s_initialized) return ESP_OK;

    if (cfg) {
        if (cfg->sda)        s_cfg.sda       = cfg->sda;
        if (cfg->scl)        s_cfg.scl       = cfg->scl;
        if (cfg->addr)       s_cfg.addr      = cfg->addr;
        if (cfg->sample_hz)  s_cfg.sample_hz = cfg->sample_hz;
        if (cfg->beta > 0)   s_cfg.beta      = cfg->beta;
    }

    /* Latch battery power BEFORE touching I2C so on-battery init has
     * a stable 3V3 rail on the sensor side of the LDO. */
    capsule_power_hold();

    /* Bring up I2C bus (new ESP-IDF v5.x driver). */
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = I2C_PORT,
        .scl_io_num        = s_cfg.scl,
        .sda_io_num        = s_cfg.sda,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t rc = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %d", rc);
        return rc;
    }

    /* Create BMI270 device against the bus. The driver internally
     * adds the device to the bus + uploads the Bosch config blob. */
    bmi270_driver_config_t imu_cfg = {
        .addr      = s_cfg.addr,
        .interface = BMI270_USE_I2C,
        .i2c_bus   = s_bus,
    };
    rc = bmi270_create(&imu_cfg, &s_bmi);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "bmi270_create: %d", rc);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return rc;
    }

    /* 50 Hz / ±4 g / ±500 dps — common AHRS defaults. ±4 g keeps the
     * accel sensitive enough for a hand-held device that doesn't see
     * sustained > 1 g, ±500 dps is well above what a person can
     * rotate the Capsule manually. */
    bmi270_config_t meas = {
        .acce_odr   = BMI270_ACC_ODR_50_HZ,
        .acce_range = BMI270_ACC_RANGE_4_G,
        .gyro_odr   = BMI270_GYR_ODR_50_HZ,
        .gyro_range = BMI270_GYR_RANGE_500_DPS,
    };
    rc = bmi270_start(s_bmi, &meas);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "bmi270_start: %d", rc);
        /* Leave bus + handle in place — caller can retry. */
        return rc;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "init ok (sda=%d scl=%d addr=0x%02x hz=%d)",
             s_cfg.sda, s_cfg.scl, s_cfg.addr, s_cfg.sample_hz);
    return ESP_OK;
}

i2c_master_bus_handle_t craw_imu_get_i2c_bus(void)
{
    return s_bus;
}

esp_err_t craw_imu_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_running)      return ESP_OK;
    s_running = true;
    /* Stack size: floats, sqrtf, log buffers — 4 KB is comfortable on
     * S3. Priority 5 keeps it above background MQTT / hive tasks (3)
     * so sample timing stays tight under bus contention. */
    BaseType_t br = xTaskCreate(sample_task, "craw_imu",
                                4096, NULL, 5, &s_task);
    if (br != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t craw_imu_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    /* Sample task self-deletes on next iteration. We don't join; the
     * caller's flow doesn't require it. */
    return ESP_OK;
}

bool craw_imu_is_running(void) { return s_running; }

void craw_imu_snapshot(craw_imu_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_latest;
    portEXIT_CRITICAL(&s_lock);
}

void craw_imu_zero(void)
{
    /* Snap the current RAW yaw (before any offset) as the new datum.
     * Compute it from the current quaternion under the lock so we
     * don't race the sample task. */
    portENTER_CRITICAL(&s_lock);
    float roll, pitch, yaw_raw;
    quat_to_euler(s_q0, s_q1, s_q2, s_q3, &roll, &pitch, &yaw_raw);
    s_yaw_zero_rad = yaw_raw;
    /* Republish the latest with yaw zeroed so a reader who polled
     * before the next sample doesn't see the old offset. */
    s_latest.yaw_rad = 0.0f;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "yaw zeroed (datum=%.3f rad)", s_yaw_zero_rad);
}

void craw_imu_stats(craw_imu_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    out->initialized    = s_initialized;
    out->running        = s_running;
    out->samples        = s_samples;
    out->last_sample_us = s_latest.timestamp_us;
    out->sample_hz      = s_cfg.sample_hz;
    out->yaw_zero_rad   = s_yaw_zero_rad;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t craw_imu_bus_scan(int sda, int scl)
{
    /* Latch power before touching I2C — same reason as craw_imu_init,
     * but `imu-scan` is the user's diagnostic before they trust init,
     * so make sure HOLD is up here too. */
    capsule_power_hold();

    i2c_master_bus_handle_t bus = NULL;
    bool own_bus = false;

    /* Reuse the existing bus if its pins match. Otherwise spin up a
     * fresh I2C_NUM_1 bus so we don't collide with a half-initialized
     * I2C_NUM_0 from a prior failed `imu-on`. */
    if (s_bus != NULL && s_cfg.sda == sda && s_cfg.scl == scl) {
        bus = s_bus;
    } else {
        i2c_master_bus_config_t bus_cfg = {
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .i2c_port          = I2C_NUM_1,
            .scl_io_num        = scl,
            .sda_io_num        = sda,
            .glitch_ignore_cnt = 7,
            .flags = { .enable_internal_pullup = true },
        };
        esp_err_t rc = i2c_new_master_bus(&bus_cfg, &bus);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus(scan, sda=%d scl=%d) → %d",
                     sda, scl, rc);
            return rc;
        }
        own_bus = true;
    }

    /* Header — i2cdetect's format. */
    printf("\r\n     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");

    /* 50 ms probe timeout per address. Total 128 × 50 ms = 6.4 s worst
     * case, but most NACKs return in well under 50 ms (the device or
     * lack thereof is on the same PCB), so a typical scan finishes in
     * under a second. */
    const uint32_t probe_timeout_ms = 50;
    int found = 0;
    for (int addr = 0x00; addr < 0x80; addr++) {
        if ((addr & 0x0F) == 0) printf("%02x: ", addr);
        if (addr < 0x03 || addr > 0x77) {
            printf("   ");
        } else {
            esp_err_t r = i2c_master_probe(bus, (uint16_t)addr, probe_timeout_ms);
            if (r == ESP_OK) { printf("%02x ", addr); found++; }
            else             { printf("-- "); }
        }
        if ((addr & 0x0F) == 0x0F) printf("\r\n");
    }
    printf("\r\nfound %d device(s) on sda=%d scl=%d\r\n", found, sda, scl);

    if (own_bus) i2c_del_master_bus(bus);
    return ESP_OK;
}
