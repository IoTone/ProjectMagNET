/* esp_attr.h — placement attributes are meaningless on the MG24 (no PSRAM, no IRAM split). */
#ifndef MN_COMPAT_ESP_ATTR_H
#define MN_COMPAT_ESP_ATTR_H
#define EXT_RAM_BSS_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#endif
