/* esp_timer.h — only esp_timer_get_time() is used outside the LED driver. */
#ifndef MN_COMPAT_ESP_TIMER_H
#define MN_COMPAT_ESP_TIMER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);      /* microseconds since boot */
#ifdef __cplusplus
}
#endif
#endif
