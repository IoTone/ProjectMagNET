/*
 * craw_imu_mpu6886 — MPU6886 sampler for M5StickC Plus.
 * Uses M5GFX's lgfx::i2c on the shared internal bus (see header).
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_imu_mpu6886.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <M5GFX.h>   // pulls in lgfx::i2c helpers

static const char *TAG = "craw_imu";

/* ---- MPU6886 register map ---- */
#define REG_SMPLRT_DIV    0x19
#define REG_CONFIG        0x1A
#define REG_GYRO_CONFIG   0x1B
#define REG_ACCEL_CONFIG  0x1C
#define REG_ACCEL_XOUT_H  0x3B
#define REG_PWR_MGMT_1    0x6B
#define REG_PWR_MGMT_2    0x6C
#define REG_WHO_AM_I      0x75

/* Sensitivities for the ranges we configure below. */
#define ACCEL_FS_LSB_PER_G   4096.0f   /* ±8g  */
#define GYRO_FS_LSB_PER_DPS  16.4f     /* ±2000 dps */
#define G_TO_MS2             9.80665f
#define DEG_TO_RAD           0.017453292519943295f

#define NVS_NS    "imu_cal"
#define NVS_KEY   "bias6"

static int      s_port = CRAW_IMU_MPU6886_I2C_PORT;
static uint8_t  s_addr = CRAW_IMU_MPU6886_ADDR;
static int      s_hz   = CRAW_IMU_MPU6886_DEFAULT_HZ;
static bool     s_initialized = false;
static bool     s_running     = false;
static bool     s_calibrated  = false;
static TaskHandle_t s_task    = nullptr;

/* Calibration offsets (subtracted from the converted reading). */
static float    s_gbx = 0, s_gby = 0, s_gbz = 0;   /* rad/s */
static float    s_abx = 0, s_aby = 0, s_abz = 0;   /* m/s²  */

/* Latest snapshot, spinlock-guarded. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static craw_imu_mpu6886_snapshot_t s_snap = {};
static uint64_t s_samples = 0;
static float    s_yaw_rad = 0;       /* integrated */
static float    s_yaw_datum = 0;     /* subtracted on read */

/* ---- I2C helpers via M5GFX (lgfx) ---- */
static bool reg_write(uint8_t reg, uint8_t val) {
    // lgfx i2c register ops return result<void> — success == has_value().
    auto r = lgfx::i2c::writeRegister8(s_port, s_addr, reg, val, 0x00, 400000);
    return r.has_value();
}

static bool reg_read(uint8_t reg, uint8_t *buf, size_t len) {
    auto r = lgfx::i2c::readRegister(s_port, s_addr, reg, buf, len, 400000);
    return r.has_value();
}

/* Read + convert one raw frame to SI (pre-calibration). */
static bool read_frame(float *ax, float *ay, float *az,
                       float *gx, float *gy, float *gz, float *tc) {
    uint8_t b[14];
    if (!reg_read(REG_ACCEL_XOUT_H, b, sizeof(b))) return false;

    int16_t raw_ax = (int16_t)((b[0]  << 8) | b[1]);
    int16_t raw_ay = (int16_t)((b[2]  << 8) | b[3]);
    int16_t raw_az = (int16_t)((b[4]  << 8) | b[5]);
    int16_t raw_t  = (int16_t)((b[6]  << 8) | b[7]);
    int16_t raw_gx = (int16_t)((b[8]  << 8) | b[9]);
    int16_t raw_gy = (int16_t)((b[10] << 8) | b[11]);
    int16_t raw_gz = (int16_t)((b[12] << 8) | b[13]);

    *ax = (raw_ax / ACCEL_FS_LSB_PER_G) * G_TO_MS2;
    *ay = (raw_ay / ACCEL_FS_LSB_PER_G) * G_TO_MS2;
    *az = (raw_az / ACCEL_FS_LSB_PER_G) * G_TO_MS2;
    *gx = (raw_gx / GYRO_FS_LSB_PER_DPS) * DEG_TO_RAD;
    *gy = (raw_gy / GYRO_FS_LSB_PER_DPS) * DEG_TO_RAD;
    *gz = (raw_gz / GYRO_FS_LSB_PER_DPS) * DEG_TO_RAD;
    /* MPU6886 temp: degC = raw/326.8 + 25.0 */
    *tc = raw_t / 326.8f + 25.0f;
    return true;
}

/* ---- NVS calibration persistence ---- */
static void cal_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    float v[6] = { s_gbx, s_gby, s_gbz, s_abx, s_aby, s_abz };
    nvs_set_blob(h, NVS_KEY, v, sizeof(v));
    nvs_commit(h);
    nvs_close(h);
}

static void cal_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    float v[6];
    size_t len = sizeof(v);
    if (nvs_get_blob(h, NVS_KEY, v, &len) == ESP_OK && len == sizeof(v)) {
        s_gbx = v[0]; s_gby = v[1]; s_gbz = v[2];
        s_abx = v[3]; s_aby = v[4]; s_abz = v[5];
        s_calibrated = true;
        ESP_LOGI(TAG, "loaded calibration from NVS");
    }
    nvs_close(h);
}

/* ---- sample task ---- */
static void sample_task(void *arg) {
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / s_hz);
    uint64_t last_us = esp_timer_get_time();
    while (s_running) {
        float ax, ay, az, gx, gy, gz, tc;
        if (read_frame(&ax, &ay, &az, &gx, &gy, &gz, &tc)) {
            /* apply calibration */
            ax -= s_abx; ay -= s_aby; az -= s_abz;
            gx -= s_gbx; gy -= s_gby; gz -= s_gbz;

            uint64_t now = esp_timer_get_time();
            float dt = (now - last_us) / 1e6f;
            last_us = now;

            /* roll/pitch from gravity (absolute), yaw integrated from gyro */
            float roll  = atan2f(ay, az);
            float pitch = atan2f(-ax, sqrtf(ay * ay + az * az));
            s_yaw_rad += gz * dt;

            float yaw = s_yaw_rad - s_yaw_datum;
            /* wrap to (-pi, pi] */
            while (yaw >  (float)M_PI) yaw -= 2.0f * (float)M_PI;
            while (yaw <= -(float)M_PI) yaw += 2.0f * (float)M_PI;

            portENTER_CRITICAL(&s_mux);
            s_snap.accel_x = ax; s_snap.accel_y = ay; s_snap.accel_z = az;
            s_snap.gyro_x  = gx; s_snap.gyro_y  = gy; s_snap.gyro_z  = gz;
            s_snap.roll_rad = roll; s_snap.pitch_rad = pitch; s_snap.yaw_rad = yaw;
            s_snap.temp_c = tc;
            s_snap.timestamp_us = now;
            s_samples++;
            portEXIT_CRITICAL(&s_mux);
        }
        vTaskDelay(period);
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

/* ---- public API ---- */
extern "C" esp_err_t craw_imu_mpu6886_init(const craw_imu_mpu6886_config_t *cfg) {
    if (s_initialized) return ESP_OK;
    if (cfg) {
        if (cfg->i2c_port)  s_port = cfg->i2c_port;
        if (cfg->addr)      s_addr = cfg->addr;
        if (cfg->sample_hz) s_hz   = cfg->sample_hz;
    }

    uint8_t who = 0;
    if (!reg_read(REG_WHO_AM_I, &who, 1)) {
        ESP_LOGE(TAG, "no I2C ACK at 0x%02x (bus owned by M5GFX? call after display.init())", s_addr);
        return ESP_ERR_NOT_FOUND;
    }
    if (who != CRAW_IMU_MPU6886_WHOAMI) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02x (expected 0x%02x) — not an MPU6886 (SH200Q variant?)",
                 who, CRAW_IMU_MPU6886_WHOAMI);
        return ESP_ERR_NOT_FOUND;
    }

    /* reset, wake, configure */
    reg_write(REG_PWR_MGMT_1, 0x80);          /* device reset */
    vTaskDelay(pdMS_TO_TICKS(100));
    reg_write(REG_PWR_MGMT_1, 0x01);          /* wake, auto clock */
    vTaskDelay(pdMS_TO_TICKS(10));
    reg_write(REG_PWR_MGMT_2, 0x00);          /* enable accel + gyro */
    reg_write(REG_ACCEL_CONFIG, 0x10);        /* ±8g  */
    reg_write(REG_GYRO_CONFIG,  0x18);        /* ±2000 dps */
    reg_write(REG_CONFIG, 0x01);              /* DLPF ~184 Hz */
    reg_write(REG_SMPLRT_DIV, 0x09);          /* 1kHz/(1+9) = 100 Hz ODR */
    vTaskDelay(pdMS_TO_TICKS(10));

    cal_load();
    s_initialized = true;
    ESP_LOGI(TAG, "MPU6886 init ok (port=%d addr=0x%02x %dHz)%s",
             s_port, s_addr, s_hz, s_calibrated ? " [cal loaded]" : "");
    return ESP_OK;
}

extern "C" esp_err_t craw_imu_mpu6886_start(void) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_OK;
    s_running = true;
    if (xTaskCreate(sample_task, "imu", 3072, nullptr, 4, &s_task) != pdPASS) {
        s_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t craw_imu_mpu6886_stop(void) {
    s_running = false;
    return ESP_OK;
}

extern "C" bool craw_imu_mpu6886_is_running(void) { return s_running; }

extern "C" esp_err_t craw_imu_mpu6886_calibrate(int samples) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (samples <= 0) samples = s_hz * 2;   /* ~2 s */

    double sgx = 0, sgy = 0, sgz = 0, sax = 0, say = 0;
    int got = 0;
    const TickType_t period = pdMS_TO_TICKS(1000 / s_hz);
    for (int i = 0; i < samples; i++) {
        float ax, ay, az, gx, gy, gz, tc;
        if (read_frame(&ax, &ay, &az, &gx, &gy, &gz, &tc)) {
            sgx += gx; sgy += gy; sgz += gz;
            sax += ax; say += ay;   /* az holds gravity — don't null it */
            got++;
        }
        vTaskDelay(period);
    }
    if (got == 0) return ESP_FAIL;

    s_gbx = (float)(sgx / got);
    s_gby = (float)(sgy / got);
    s_gbz = (float)(sgz / got);
    s_abx = (float)(sax / got);
    s_aby = (float)(say / got);
    s_abz = 0.0f;                   /* keep Z gravity */

    /* reset integrated yaw + datum since bias just changed */
    portENTER_CRITICAL(&s_mux);
    s_yaw_rad = 0; s_yaw_datum = 0;
    portEXIT_CRITICAL(&s_mux);

    s_calibrated = true;
    cal_save();
    ESP_LOGI(TAG, "calibrated over %d samples: gyro bias=(%.4f,%.4f,%.4f) rad/s",
             got, s_gbx, s_gby, s_gbz);
    return ESP_OK;
}

extern "C" bool craw_imu_mpu6886_is_calibrated(void) { return s_calibrated; }

extern "C" void craw_imu_mpu6886_clear_cal(void) {
    s_gbx = s_gby = s_gbz = 0;
    s_abx = s_aby = s_abz = 0;
    s_calibrated = false;
    portENTER_CRITICAL(&s_mux);
    s_yaw_rad = 0; s_yaw_datum = 0;
    portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "calibration cleared");
}

extern "C" void craw_imu_mpu6886_snapshot(craw_imu_mpu6886_snapshot_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_snap;
    portEXIT_CRITICAL(&s_mux);
}

extern "C" void craw_imu_mpu6886_zero_yaw(void) {
    portENTER_CRITICAL(&s_mux);
    s_yaw_datum = s_yaw_rad;
    portEXIT_CRITICAL(&s_mux);
}

extern "C" void craw_imu_mpu6886_stats(craw_imu_mpu6886_stats_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    out->samples = s_samples;
    portEXIT_CRITICAL(&s_mux);
    out->initialized = s_initialized;
    out->running     = s_running;
    out->calibrated  = s_calibrated;
    out->sample_hz   = s_hz;
    out->gyro_bias_x = s_gbx; out->gyro_bias_y = s_gby; out->gyro_bias_z = s_gbz;
    out->accel_bias_x = s_abx; out->accel_bias_y = s_aby; out->accel_bias_z = s_abz;
}
