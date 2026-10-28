// presence — see presence.h.

#include "presence.h"

void presence_init(presence_t *p, int64_t now_ms,
                   uint32_t hold_s, uint32_t repeat_s, uint32_t warmup_s) {
    *p = (presence_t){
        .hold_s = hold_s, .repeat_s = repeat_s, .warmup_s = warmup_s,
        .state = warmup_s ? PRES_WARMUP : PRES_CLEAR,
        .t_start_ms = now_ms,
    };
}

pres_event_t presence_step(presence_t *p, int64_t now_ms, bool motion) {
    p->motion = motion;
    if (p->state == PRES_WARMUP) {
        // A PIR swings its output while the pyro element settles; ignore it.
        if (now_ms - p->t_start_ms < (int64_t)p->warmup_s * 1000) return PRES_EV_NONE;
        p->state = PRES_CLEAR;
        return PRES_EV_READY;
    }
    if (motion) p->t_motion_ms = now_ms;

    if (p->state == PRES_CLEAR) {
        if (!motion) return PRES_EV_NONE;
        p->state = PRES_PRESENT;
        p->t_enter_ms = p->t_ping_ms = now_ms;
        return PRES_EV_ENTER;
    }

    if (now_ms - p->t_motion_ms >= (int64_t)p->hold_s * 1000) {
        p->state = PRES_CLEAR;
        return PRES_EV_CLEAR;
    }
    if (now_ms - p->t_ping_ms >= (int64_t)p->repeat_s * 1000) {
        p->t_ping_ms = now_ms;
        return PRES_EV_STILL;
    }
    return PRES_EV_NONE;
}

const char *presence_state_name(pres_state_t s) {
    switch (s) {
    case PRES_WARMUP:  return "warmup";
    case PRES_CLEAR:   return "clear";
    case PRES_PRESENT: return "present";
    }
    return "?";
}

const char *presence_event_name(pres_event_t e) {
    switch (e) {
    case PRES_EV_NONE:  return "none";
    case PRES_EV_READY: return "ready";
    case PRES_EV_ENTER: return "enter";
    case PRES_EV_STILL: return "present";
    case PRES_EV_CLEAR: return "clear";
    }
    return "?";
}
