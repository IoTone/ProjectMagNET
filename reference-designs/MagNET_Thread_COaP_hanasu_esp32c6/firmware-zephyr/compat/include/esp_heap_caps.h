/* esp_heap_caps.h — reports the libc malloc arena (where the Forth heap and
 * every MagNET malloc live). The caps argument is ignored: one kind of RAM. */
#ifndef MN_COMPAT_ESP_HEAP_CAPS_H
#define MN_COMPAT_ESP_HEAP_CAPS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DEFAULT  (1u << 12)
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
#ifdef __cplusplus
}
#endif
#endif
