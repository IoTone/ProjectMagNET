/*
 * craw_bh1750 — minimal driver for the Rohm BH1750 ambient light sensor.
 *
 * I²C address 0x23 (default) or 0x5C (alt).
 * Continuous high-resolution mode → 1 lux precision, ~120 ms typ. conversion.
 *
 * Public API is two functions: init the bus + device, then read lux.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Init I²C0 master + BH1750 device, put it into continuous H-res mode. */
esp_err_t craw_bh1750_init(int sda_gpio, int scl_gpio);

/** Read latest lux value. Returns ESP_OK on success. */
esp_err_t craw_bh1750_read(float *out_lux);

/** Tear down (rare). */
void craw_bh1750_deinit(void);

#ifdef __cplusplus
}
#endif
