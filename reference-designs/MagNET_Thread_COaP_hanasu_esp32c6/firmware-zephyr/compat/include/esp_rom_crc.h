/* esp_rom_crc.h — esp_rom_crc32_le(0, …) is standard CRC-32 (zlib.crc32,
 * what the bundle signing tool computes), which is exactly Zephyr's
 * crc32_ieee. Chaining semantics match zlib too: pass the previous result as
 * crc to continue. Pinned by the "123456789" → 0xCBF43926 power-on KAT. */
#ifndef MN_COMPAT_ESP_ROM_CRC_H
#define MN_COMPAT_ESP_ROM_CRC_H
#include <stdint.h>
#include <zephyr/sys/crc.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) {
    return crc32_ieee_update(crc, buf, len);
}
#endif
