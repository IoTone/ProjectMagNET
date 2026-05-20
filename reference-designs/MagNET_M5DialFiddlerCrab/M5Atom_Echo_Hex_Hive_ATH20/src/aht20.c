/*
 * aht20.c - AHT20 I²C driver (raw, no Arduino libs)
 *
 * Protocol (datasheet rev 1.1):
 *   Power-on settle:    ~40 ms after VCC up
 *   Calibrate (once):   write [0xBE, 0x08, 0x00]
 *                       wait 10 ms
 *                       read 1 status byte; bit 3 must be 1 (calibrated)
 *   Trigger measurement: write [0xAC, 0x33, 0x00]
 *                        wait 80 ms (or poll status until bit 7 clears)
 *   Read 7 bytes:       [status, h[19..12], h[11..4],
 *                        h[3..0]|t[19..16], t[15..8], t[7..0], crc]
 *   RH%  = h / 2^20 * 100
 *   T °C = t / 2^20 * 200 - 50
 */

#include "aht20.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "aht20";

#define AHT20_ADDR        0x38
#define AHT20_I2C_HZ      100000
#define AHT20_OP_TIMEOUT  100

static i2c_master_bus_handle_t s_bus  = NULL;
static i2c_master_dev_handle_t s_dev  = NULL;
static bool                    s_ready = false;

esp_err_t aht20_init(int sda_gpio, int scl_gpio) {
    if (s_ready) return ESP_OK;
    esp_err_t err;

    /* Retry-safe: bus + device handles persist across failed probes
     * (so `i2c-scan` keeps working). Only create them once. */
    if (!s_bus) {
        i2c_master_bus_config_t bus_cfg = {
            .clk_source                   = I2C_CLK_SRC_DEFAULT,
            .i2c_port                     = I2C_NUM_0,
            .sda_io_num                   = sda_gpio,
            .scl_io_num                   = scl_gpio,
            .glitch_ignore_cnt            = 7,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (!s_dev) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = AHT20_ADDR,
            .scl_speed_hz    = AHT20_I2C_HZ,
        };
        err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* Power-on settle */
    vTaskDelay(pdMS_TO_TICKS(40));

    /* Probe — confirm the sensor is on the bus before sending init cmds. */
    err = i2c_master_probe(s_bus, AHT20_ADDR, AHT20_OP_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no AHT20 at 0x%02x (probe: %s)",
                 AHT20_ADDR, esp_err_to_name(err));
        return err;
    }

    /* Calibration (one-shot at boot). */
    const uint8_t cal[] = { 0xBE, 0x08, 0x00 };
    err = i2c_master_transmit(s_dev, cal, sizeof(cal), AHT20_OP_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "calibrate write: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t status = 0;
    err = i2c_master_receive(s_dev, &status, 1, AHT20_OP_TIMEOUT);
    if (err == ESP_OK && !(status & 0x08))
        ESP_LOGW(TAG, "AHT20 reports uncalibrated (status=0x%02x)", status);

    s_ready = true;
    ESP_LOGI(TAG, "AHT20 ready @ 0x38 on SDA=%d SCL=%d (status=0x%02x)",
             sda_gpio, scl_gpio, status);
    return ESP_OK;
}

esp_err_t aht20_read(float *t_c, float *rh_pct) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!t_c || !rh_pct) return ESP_ERR_INVALID_ARG;

    /* Trigger one measurement. */
    const uint8_t trig[] = { 0xAC, 0x33, 0x00 };
    esp_err_t err = i2c_master_transmit(s_dev, trig, sizeof(trig), AHT20_OP_TIMEOUT);
    if (err != ESP_OK) return err;

    /* Datasheet says wait ~80 ms; sensor will assert busy in status[7] until
     * the conversion is done. Poll briefly to avoid hard-coding too long. */
    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t buf[7] = {0};
    err = i2c_master_receive(s_dev, buf, sizeof(buf), AHT20_OP_TIMEOUT);
    if (err != ESP_OK) return err;
    if (buf[0] & 0x80) {   /* still busy — give it one more tick */
        vTaskDelay(pdMS_TO_TICKS(20));
        err = i2c_master_receive(s_dev, buf, sizeof(buf), AHT20_OP_TIMEOUT);
        if (err != ESP_OK) return err;
        if (buf[0] & 0x80) return ESP_ERR_TIMEOUT;
    }

    uint32_t hum_raw  = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) |
                        ((uint32_t)buf[3] >> 4);
    uint32_t temp_raw = ((uint32_t)(buf[3] & 0x0F) << 16) |
                        ((uint32_t)buf[4] << 8) | (uint32_t)buf[5];

    *rh_pct = (float)hum_raw  * 100.0f  / 1048576.0f;          /* 2^20 */
    *t_c    = (float)temp_raw * 200.0f  / 1048576.0f - 50.0f;
    return ESP_OK;
}

void aht20_scan_bus(void) {
    if (!s_bus) { ESP_LOGW(TAG, "scan: bus not initialized"); return; }
    ESP_LOGI(TAG, "i2c scan starting (0x08..0x77)");
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  responder at 0x%02x", a);
            found++;
        }
    }
    if (found == 0) ESP_LOGW(TAG, "i2c scan: no devices — check SDA/SCL wiring");
    else ESP_LOGI(TAG, "i2c scan: %d device(s)", found);
}

