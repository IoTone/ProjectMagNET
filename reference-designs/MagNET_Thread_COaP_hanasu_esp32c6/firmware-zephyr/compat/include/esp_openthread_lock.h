/* esp_openthread_lock.h — the OT API lock is Zephyr's openthread_mutex. */
#ifndef MN_COMPAT_ESP_OPENTHREAD_LOCK_H
#define MN_COMPAT_ESP_OPENTHREAD_LOCK_H
#include <stdbool.h>
#include <openthread.h>
#include "freertos/FreeRTOS.h"
static inline bool esp_openthread_lock_acquire(TickType_t wait) {
    (void)wait;                 /* every MagNET caller passes portMAX_DELAY */
    openthread_mutex_lock();
    return true;
}
static inline void esp_openthread_lock_release(void) { openthread_mutex_unlock(); }
#endif
