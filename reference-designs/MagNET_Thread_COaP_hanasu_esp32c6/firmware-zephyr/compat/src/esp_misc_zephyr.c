/*
 * esp_misc_zephyr.c — time, randomness, reboot, heap/chip/flash queries and
 * flat-numbered GPIO for the ESP-IDF compat layer.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/version.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "driver/gpio.h"

/* lib/libc/common: stats for the malloc arena (needs SYS_HEAP_RUNTIME_STATS) */
int malloc_runtime_stats_get(struct sys_memory_stats *stats);

int64_t esp_timer_get_time(void) {
    return (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

/* ---- randomness: CSPRNG (Secure Engine TRNG seeded) ---- */
uint32_t esp_random(void) {
    uint32_t v;
    if (sys_csrand_get(&v, sizeof(v)) != 0) v = sys_rand32_get();
    return v;
}

void esp_fill_random(void *buf, size_t len) {
    if (sys_csrand_get(buf, len) != 0) sys_rand_get(buf, len);
}

/* ---- system ---- */
void esp_restart(void) {
    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

esp_reset_reason_t esp_reset_reason(void) {
    uint32_t cause = 0;
    (void)hwinfo_get_reset_cause(&cause);
    return (esp_reset_reason_t)cause;       /* RESET_* bits, not IDF enum values */
}

const char *esp_get_idf_version(void) {
    return "zephyr-" KERNEL_VERSION_STRING;
}

/* ---- heap: the libc malloc arena (Forth heap + every MagNET malloc) ----
 * sys_heap keeps no largest-free-block figure, so "largest" reports total
 * free — an upper bound, flagged as such in SYSINFO comparisons. */
static struct sys_memory_stats heap_stats(void) {
    struct sys_memory_stats s = {0};
    (void)malloc_runtime_stats_get(&s);
    return s;
}

size_t heap_caps_get_free_size(uint32_t caps) { ARG_UNUSED(caps); return heap_stats().free_bytes; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { ARG_UNUSED(caps); return heap_stats().free_bytes; }

size_t heap_caps_get_minimum_free_size(uint32_t caps) {
    ARG_UNUSED(caps);
    struct sys_memory_stats s = heap_stats();
    size_t total = s.free_bytes + s.allocated_bytes;
    return total - s.max_allocated_bytes;
}

uint32_t esp_get_free_heap_size(void) { return (uint32_t)heap_stats().free_bytes; }

/* ---- chip / flash / mac ---- */
void esp_chip_info(esp_chip_info_t *out) {
    memset(out, 0, sizeof(*out));
    out->model = CHIP_EFR32MG24;
    out->cores = 1;
    out->features = CHIP_FEATURE_EMB_FLASH | CHIP_FEATURE_BLE | CHIP_FEATURE_IEEE802154;
}

esp_err_t esp_flash_get_size(void *chip, uint32_t *out_size) {
    ARG_UNUSED(chip);
    *out_size = DT_REG_SIZE(DT_NODELABEL(flash0));
    return ESP_OK;
}

/* The MG24's factory EUI-64 is the hwinfo device id; like an ESP MAC, take
 * the 3-byte OUI and the last 3 (NIC) bytes. */
esp_err_t esp_efuse_mac_get_default(uint8_t mac[6]) {
    uint8_t eui[8] = {0};
    if (hwinfo_get_device_id(eui, sizeof(eui)) < 8) return ESP_FAIL;
    memcpy(mac, eui, 3);
    memcpy(mac + 3, eui + 5, 3);
    return ESP_OK;
}

/* ---- GPIO: flat pin = port*16 + pin (PA0=0, PB0=16, PC0=32, PD0=48) ---- */
static const struct device *const s_ports[] = {
    DEVICE_DT_GET(DT_NODELABEL(gpioa)),
    DEVICE_DT_GET(DT_NODELABEL(gpiob)),
    DEVICE_DT_GET(DT_NODELABEL(gpioc)),
    DEVICE_DT_GET(DT_NODELABEL(gpiod)),
};

static const struct device *port_of(gpio_num_t pin) {
    if (pin < 0 || pin >= 16 * (int)ARRAY_SIZE(s_ports)) return NULL;
    const struct device *d = s_ports[pin / 16];
    return device_is_ready(d) ? d : NULL;
}

esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) {
    const struct device *d = port_of(pin);
    if (!d) return ESP_ERR_INVALID_ARG;
    gpio_flags_t f = (mode == GPIO_MODE_OUTPUT) ? GPIO_OUTPUT_INACTIVE : GPIO_INPUT;
    return gpio_pin_configure(d, pin % 16, f) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
    const struct device *d = port_of(pin);
    if (!d) return ESP_ERR_INVALID_ARG;
    return gpio_pin_set_raw(d, pin % 16, level ? 1 : 0) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t gpio_reset_pin(gpio_num_t pin) {
    const struct device *d = port_of(pin);
    if (!d) return ESP_ERR_INVALID_ARG;
    return gpio_pin_configure(d, pin % 16, GPIO_DISCONNECTED) == 0 ? ESP_OK : ESP_FAIL;
}
