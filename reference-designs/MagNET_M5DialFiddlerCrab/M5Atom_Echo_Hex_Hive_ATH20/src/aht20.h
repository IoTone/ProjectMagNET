/*
 * aht20.h - minimal raw I²C driver for the Adafruit AHT20 breakout
 *
 * The AHT20 protocol is trivial enough that pulling in an Arduino-style
 * wrapper library would be net-negative complexity. This is ~80 lines
 * of straight ESP-IDF v5 i2c_master.h calls.
 *
 *   aht20_init(sda_gpio, scl_gpio)   -> bring the bus up + send the
 *                                        calibration command (one-shot)
 *   aht20_read(&t_c, &rh_pct)        -> one measurement (~80 ms blocking)
 *
 * Notes:
 *   - Sensor address is fixed at 0x38 (no jumper). One AHT20 per bus.
 *   - The breakout has internal pull-ups; no externals needed.
 *   - Self-heating above ~1 Hz polling can drift the reading +0.5 °C.
 *     Caller should sample at 0.5 Hz or slower.
 */
#ifndef AHT20_H
#define AHT20_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t aht20_init(int sda_gpio, int scl_gpio);
esp_err_t aht20_read(float *t_c, float *rh_pct);

/* Bus diagnostic: probe 0x08..0x77 and ESP_LOGI any responder. The bus
 * stays initialized after aht20_init() returns even on probe failure, so
 * this works from the REPL to tell wiring problems from address problems. */
void aht20_scan_bus(void);

#endif /* AHT20_H */
