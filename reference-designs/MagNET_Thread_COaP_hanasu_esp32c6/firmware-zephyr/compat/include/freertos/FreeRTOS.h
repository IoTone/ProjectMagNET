/*
 * FreeRTOS.h — the slice of the FreeRTOS API the MagNET sources use, on Zephyr.
 *
 * Why a shim and not an mn_port rewrite: the IDF sources are hardware-validated
 * through E-H, and this lets the Zephyr build compile them unmodified. Only
 * what those files actually call is here; anything else fails to link, loudly.
 *
 * Semantics that differ from FreeRTOS, and how they are closed:
 *   - Ticks: TickType_t is milliseconds (pdMS_TO_TICKS is the identity), so
 *     tick math in the sources means the same thing it did at 1 kHz on IDF.
 *   - Priorities: FreeRTOS is higher-number-wins, Zephyr lower-number-wins;
 *     xTaskCreate maps 0..(N-1) onto the preemptible range inverted.
 *   - Stack sizes: IDF's xTaskCreate takes BYTES (not words) — same here.
 *   - Timer callbacks: FreeRTOS runs them on the timer-service TASK; a raw
 *     k_timer expiry runs in ISR context. Expiries are bounced onto a
 *     dedicated work queue so callbacks keep thread context.
 *   - Mutexes: k_mutex is always recursive, so plain and recursive mutexes
 *     are the same object (a plain FreeRTOS mutex re-taken by its holder
 *     would deadlock; nothing in MagNET relies on that).
 */
#ifndef MN_COMPAT_FREERTOS_H
#define MN_COMPAT_FREERTOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;

#define portMAX_DELAY        ((TickType_t)0xffffffffu)
#define portTICK_PERIOD_MS   1
#define configTICK_RATE_HZ   1000     /* ticks are ms (see above) */
#define pdMS_TO_TICKS(ms)    ((TickType_t)(ms))
#define pdTICKS_TO_MS(t)     ((uint32_t)(t))
#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0
#define configASSERT(x)      do { if (!(x)) mn_compat_panic(__FILE__, __LINE__); } while (0)

void mn_compat_panic(const char *file, int line);

/* ---- tasks ---- */
typedef struct k_thread *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t   xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                         void *arg, UBaseType_t prio, TaskHandle_t *out);
void         vTaskDelete(TaskHandle_t t);           /* NULL = self; stack reaped */
void         vTaskDelay(TickType_t ticks);
TickType_t   xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void         taskYIELD(void);

/* ---- semaphores / mutexes ---- */
typedef struct mn_sem *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);
TaskHandle_t      xSemaphoreGetMutexHolder(SemaphoreHandle_t s);
#define xSemaphoreTakeRecursive(s, w) xSemaphoreTake((s), (w))
#define xSemaphoreGiveRecursive(s)    xSemaphoreGive(s)

/* ---- queues (copy semantics, same as FreeRTOS) ---- */
typedef struct k_msgq *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
BaseType_t    xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t    xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
UBaseType_t   uxQueueMessagesWaiting(QueueHandle_t q);
#define xQueueSendToBack(q, i, w) xQueueSend((q), (i), (w))

/* ---- software timers ---- */
typedef struct mn_timer *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);

TimerHandle_t xTimerCreate(const char *name, TickType_t period, UBaseType_t auto_reload,
                           void *id, TimerCallbackFunction_t cb);
BaseType_t    xTimerStart(TimerHandle_t t, TickType_t wait);
BaseType_t    xTimerStop(TimerHandle_t t, TickType_t wait);
BaseType_t    xTimerChangePeriod(TimerHandle_t t, TickType_t period, TickType_t wait);
BaseType_t    xTimerIsTimerActive(TimerHandle_t t);
void         *pvTimerGetTimerID(TimerHandle_t t);

#ifdef __cplusplus
}
#endif

#endif /* MN_COMPAT_FREERTOS_H */
