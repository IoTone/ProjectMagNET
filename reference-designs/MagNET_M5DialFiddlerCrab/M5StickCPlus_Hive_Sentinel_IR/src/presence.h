#ifndef PRESENCE_H
#define PRESENCE_H

/* presence — PIR motion → people-presence state machine. Pure C, no IDF:
 * time is passed in (monotonic ms) so the logic is host-testable
 * (tools/test_logic.c).
 *
 *   WARMUP ──(warmup_s elapsed)──▶ CLEAR ──(motion)──▶ PRESENT
 *                                    ▲                    │
 *                                    └──(no motion hold_s)┘
 *
 * Events: ENTER on CLEAR→PRESENT (one per room entry), STILL every
 * repeat_s while motion keeps arriving within hold_s, CLEAR on the way out.
 * CLEAR wins over STILL when both fall due on the same step.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum { PRES_WARMUP, PRES_CLEAR, PRES_PRESENT } pres_state_t;

typedef enum {
    PRES_EV_NONE,
    PRES_EV_READY,     /* warm-up finished, sensor trusted */
    PRES_EV_ENTER,
    PRES_EV_STILL,
    PRES_EV_CLEAR,
} pres_event_t;

typedef struct {
    uint32_t     hold_s;      /* no motion this long = room clear */
    uint32_t     repeat_s;    /* re-ping interval while present */
    uint32_t     warmup_s;    /* PIR settle time after power-up */
    pres_state_t state;
    bool         motion;      /* last input sample */
    int64_t      t_start_ms;
    int64_t      t_enter_ms;
    int64_t      t_motion_ms; /* last time motion was seen */
    int64_t      t_ping_ms;   /* last ENTER/STILL */
} presence_t;

void         presence_init(presence_t *p, int64_t now_ms,
                           uint32_t hold_s, uint32_t repeat_s, uint32_t warmup_s);
/* Feed one (debounced) sample. Returns at most one event. */
pres_event_t presence_step(presence_t *p, int64_t now_ms, bool motion);
const char  *presence_state_name(pres_state_t s);
const char  *presence_event_name(pres_event_t e);

#endif
