/*
 * freertos_zephyr.c — FreeRTOS subset on Zephyr kernel objects.
 * See compat/include/freertos/FreeRTOS.h for the semantic notes.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

static k_timeout_t to_timeout(TickType_t t) {
    return (t == portMAX_DELAY) ? K_FOREVER : K_MSEC(t);
}

void mn_compat_panic(const char *file, int line) {
    printk("\r\n-ERR E_INTERNAL assert %s:%d\r\n", file, line);
    k_panic();
}

/* ============================== tasks =================================== */

#define MN_TASK_MAGIC 0x4d4e544bu            /* "MNTK" */

struct mn_task {
    struct k_thread   thread;                /* first: k_current_get() → task */
    uint32_t          magic;
    k_thread_stack_t *stack;
    TaskFunction_t    fn;
    void             *arg;
    struct mn_task   *next_zombie;
};

/* A thread can't free its own stack, so a self-deleted task parks here and
 * the next xTaskCreate reaps it. MagNET deletes tasks rarely (a finished
 * STRESS run), so lazy reaping costs nothing and needs no reaper thread. */
static struct mn_task *s_zombies;
static struct k_spinlock s_zombie_lock;

static void reap_zombies(void) {
    k_spinlock_key_t key = k_spin_lock(&s_zombie_lock);
    struct mn_task **pp = &s_zombies;
    while (*pp) {
        struct mn_task *t = *pp;
        if (k_thread_join(&t->thread, K_NO_WAIT) == 0) {
            *pp = t->next_zombie;
            k_spin_unlock(&s_zombie_lock, key);
            k_thread_stack_free(t->stack);
            k_free(t);
            key = k_spin_lock(&s_zombie_lock);
        } else {
            pp = &t->next_zombie;
        }
    }
    k_spin_unlock(&s_zombie_lock, key);
}

static void task_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p2); ARG_UNUSED(p3);
    struct mn_task *t = p1;
    t->fn(t->arg);
    vTaskDelete(NULL);                       /* FreeRTOS tasks must not return */
}

/* FreeRTOS: higher number = more urgent. Zephyr preemptible: lower = more
 * urgent, 0..CONFIG_NUM_PREEMPT_PRIORITIES-1. Invert, clamp. */
static int map_prio(UBaseType_t prio) {
    int z = (CONFIG_NUM_PREEMPT_PRIORITIES - 1) - (int)prio;
    if (z < 0) z = 0;
    if (z > CONFIG_NUM_PREEMPT_PRIORITIES - 1) z = CONFIG_NUM_PREEMPT_PRIORITIES - 1;
    return K_PRIO_PREEMPT(z);
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                       void *arg, UBaseType_t prio, TaskHandle_t *out) {
    reap_zombies();
    struct mn_task *t = k_calloc(1, sizeof(*t));
    if (!t) return pdFAIL;
    t->stack = k_thread_stack_alloc(stack_bytes, 0);
    if (!t->stack) { k_free(t); return pdFAIL; }
    t->magic = MN_TASK_MAGIC;
    t->fn = fn;
    t->arg = arg;
    k_tid_t tid = k_thread_create(&t->thread, t->stack, stack_bytes, task_entry,
                                  t, NULL, NULL, map_prio(prio), 0, K_NO_WAIT);
    if (name) k_thread_name_set(tid, name);
    if (out) *out = tid;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t h) {
    struct k_thread *self = k_current_get();
    struct k_thread *target = h ? h : self;
    struct mn_task *t = (struct mn_task *)target;   /* thread is first member */
    if (t->magic == MN_TASK_MAGIC) {
        k_spinlock_key_t key = k_spin_lock(&s_zombie_lock);
        t->next_zombie = s_zombies;
        s_zombies = t;
        k_spin_unlock(&s_zombie_lock, key);
    }
    k_thread_abort(target);
}

void vTaskDelay(TickType_t ticks) { k_sleep(to_timeout(ticks)); }
TickType_t xTaskGetTickCount(void) { return k_uptime_get_32(); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return k_current_get(); }
void taskYIELD(void) { k_yield(); }

/* ============================ semaphores ================================= */

struct mn_sem {
    bool is_mutex;
    union { struct k_mutex m; struct k_sem s; };
};

static SemaphoreHandle_t mutex_new(void) {
    struct mn_sem *s = k_calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->is_mutex = true;
    k_mutex_init(&s->m);
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return mutex_new(); }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return mutex_new(); }

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    struct mn_sem *s = k_calloc(1, sizeof(*s));
    if (!s) return NULL;
    k_sem_init(&s->s, 0, 1);                 /* FreeRTOS binary starts empty */
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    int rc = s->is_mutex ? k_mutex_lock(&s->m, to_timeout(wait))
                         : k_sem_take(&s->s, to_timeout(wait));
    return rc == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (s->is_mutex) return k_mutex_unlock(&s->m) == 0 ? pdTRUE : pdFALSE;
    k_sem_give(&s->s);
    return pdTRUE;
}

TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t s) {
    return s->is_mutex ? s->m.owner : NULL;
}

/* ============================== queues =================================== */

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size) {
    struct k_msgq *q = k_calloc(1, sizeof(*q));
    char *buf = k_aligned_alloc(8, (size_t)len * item_size);
    if (!q || !buf) { k_free(q); k_free(buf); return NULL; }
    k_msgq_init(q, buf, item_size, len);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
    return k_msgq_put(q, item, to_timeout(wait)) == 0 ? pdPASS : pdFAIL;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) {
    return k_msgq_get(q, item, to_timeout(wait)) == 0 ? pdPASS : pdFAIL;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return k_msgq_num_used_get(q); }

/* ============================== timers =================================== */

/* The FreeRTOS "timer service task": expiries (ISR context) are bounced here
 * so callbacks run in a thread, as MagNET's callbacks assume. IDF runs that
 * task at priority 1; mapped the same way as every other task. */
#define MN_TMR_STACK 3072
K_THREAD_STACK_DEFINE(s_tmr_stack, MN_TMR_STACK);
static struct k_work_q s_tmr_q;
static bool s_tmr_q_up;

struct mn_timer {
    struct k_timer          kt;
    struct k_work           work;
    TimerCallbackFunction_t cb;
    void                   *id;
    uint32_t                period_ms;
    bool                    reload;
    atomic_t                active;
};

static void tmr_work(struct k_work *w) {
    struct mn_timer *t = CONTAINER_OF(w, struct mn_timer, work);
    t->cb(t);
}

static void tmr_expiry(struct k_timer *kt) {
    struct mn_timer *t = CONTAINER_OF(kt, struct mn_timer, kt);
    if (!t->reload) atomic_clear(&t->active);
    k_work_submit_to_queue(&s_tmr_q, &t->work);
}

TimerHandle_t xTimerCreate(const char *name, TickType_t period, UBaseType_t auto_reload,
                           void *id, TimerCallbackFunction_t cb) {
    ARG_UNUSED(name);
    if (!s_tmr_q_up) {                       /* first timer: start the service q */
        struct k_work_queue_config cfg = { .name = "mn_tmr_svc" };
        k_work_queue_start(&s_tmr_q, s_tmr_stack, K_THREAD_STACK_SIZEOF(s_tmr_stack),
                           map_prio(1), &cfg);
        s_tmr_q_up = true;
    }
    struct mn_timer *t = k_calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cb = cb;
    t->id = id;
    t->period_ms = period;
    t->reload = auto_reload;
    k_timer_init(&t->kt, tmr_expiry, NULL);
    k_work_init(&t->work, tmr_work);
    return t;
}

BaseType_t xTimerStart(TimerHandle_t t, TickType_t wait) {
    ARG_UNUSED(wait);
    atomic_set(&t->active, 1);
    k_timer_start(&t->kt, K_MSEC(t->period_ms),
                  t->reload ? K_MSEC(t->period_ms) : K_NO_WAIT);
    return pdPASS;
}

BaseType_t xTimerStop(TimerHandle_t t, TickType_t wait) {
    ARG_UNUSED(wait);
    atomic_clear(&t->active);
    k_timer_stop(&t->kt);
    return pdPASS;
}

/* Like FreeRTOS, changing the period also (re)starts a dormant timer. */
BaseType_t xTimerChangePeriod(TimerHandle_t t, TickType_t period, TickType_t wait) {
    t->period_ms = period;
    return xTimerStart(t, wait);
}

BaseType_t xTimerIsTimerActive(TimerHandle_t t) { return atomic_get(&t->active) ? pdTRUE : pdFALSE; }
void *pvTimerGetTimerID(TimerHandle_t t) { return t->id; }
