/* driver/gpio.h — flat pin numbers for the Forth gpio-* words.
 * MG24 pins are (port, pin); flat = port*16 + pin, so PA7 (the XIAO's user
 * LED) is 7, PB0 is 16, PC0 is 32, PD0 is 48. */
#ifndef MN_COMPAT_DRIVER_GPIO_H
#define MN_COMPAT_DRIVER_GPIO_H
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
esp_err_t gpio_reset_pin(gpio_num_t pin);
#ifdef __cplusplus
}
#endif
#endif
