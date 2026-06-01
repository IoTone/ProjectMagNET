#include "craw_bh1750.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "craw_bh1750";

#define BH1750_I2C_ADDR     0x23
#define BH1750_I2C_SPEED_HZ 100000

/* BH1750 opcodes */
#define BH1750_POWER_ON    0x01
#define BH1750_RESET       0x07
#define BH1750_CONT_H_RES  0x10  /* 1 lux precision, ~120 ms typ. conversion */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t write_cmd(uint8_t op) {
    return i2c_master_transmit(s_dev, &op, 1, 100);
}

esp_err_t craw_bh1750_init(int sda_gpio, int scl_gpio) {
    if (s_bus != NULL) return ESP_ERR_INVALID_STATE;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BH1750_I2C_ADDR,
        .scl_speed_hz    = BH1750_I2C_SPEED_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device: %s", esp_err_to_name(err));
        goto fail_bus;
    }

    if ((err = write_cmd(BH1750_POWER_ON))   != ESP_OK) goto fail_dev;
    if ((err = write_cmd(BH1750_RESET))      != ESP_OK) goto fail_dev;
    if ((err = write_cmd(BH1750_CONT_H_RES)) != ESP_OK) goto fail_dev;

    /* First conversion takes ~180 ms in worst case; let one finish before the
     * caller starts reading. Doesn't block subsequent reads. */
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "init ok sda=%d scl=%d addr=0x%02X", sda_gpio, scl_gpio, BH1750_I2C_ADDR);
    return ESP_OK;

fail_dev:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
fail_bus:
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    return err;
}

esp_err_t craw_bh1750_read(float *out_lux) {
    if (!s_dev || !out_lux) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2];
    esp_err_t err = i2c_master_receive(s_dev, buf, 2, 200);
    if (err != ESP_OK) return err;
    uint16_t raw = ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
    /* Datasheet conversion factor is 1.2 to get lux from H-resolution counts. */
    *out_lux = (float)raw / 1.2f;
    return ESP_OK;
}

void craw_bh1750_deinit(void) {
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }
}
