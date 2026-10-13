/*
 * magnet_bot.c — bot mode: opt-in auto-responders for bench and demo use.
 *
 * Why C and not Forth: the ESPIDFORTH engine this firmware embeds has no
 * string primitives beyond the `str=` FFI word (no compare/search/move/count),
 * no create/does> for tables, no execute/' for dispatch, and 4096 cells of
 * compiled-code space shared by every definition. Anything that has to take
 * incoming text apart belongs down here; Forth stays the control surface, the
 * same split the mn-* vocabulary already uses (§12.3).
 *
 * THE LOOP PROBLEM is the whole design. A responder that answers every chat
 * will, on a mesh where two nodes both have one armed, volley forever: A's
 * reply is B's stimulus and vice versa. The E-E bench test already found this
 * with Forth hooks and capped them at 5/s, but 5 messages a second sustained
 * still ruins a channel. Four independent guards, cheapest first:
 *
 *   1. MN_F_AUTOMATED — replies are marked; bots never answer a marked frame.
 *      Deterministically breaks bot<->bot. The other three exist because this
 *      one trusts the sender, and a node running older or other firmware
 *      cannot be trusted to set it.
 *   2. Never answer ourselves (multicast loopback / our own id).
 *   3. Per-peer cooldown — at most one reply to a given sender per 30 s.
 *   4. Global budget — MN_BOT_MAX_PER_MIN replies a minute across all peers,
 *      the backstop for "twelve peers all say hi at once".
 *
 * Bot mode is per-boot state on purpose: it is a test mode, and a node that
 * resumes talking to the channel by itself after a power cycle is a surprise
 * nobody wants at 3 a.m.
 */
#include "magnet_bot.h"

#if MN_ENABLE_BOTS

#include "magnet.h"
#include "magnet_envelope.h"

#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

/* ---- guards ---- */
#define MN_BOT_PEER_COOLDOWN_US  (30 * 1000000LL)  /* per sender */
#define MN_BOT_MAX_PER_MIN       6                 /* across all senders */
#define MN_BOT_COOLDOWN_SLOTS    8

static struct { uint8_t id[4]; int64_t last_us; bool used; }
    s_cool[MN_BOT_COOLDOWN_SLOTS];
static int64_t s_budget_window_us = 0;
static int     s_budget = MN_BOT_MAX_PER_MIN;
static bool    s_budget_warned = false;

/* Returns true if we may answer this sender right now, and records the reply.
 * Only called once the bot has decided it actually wants to speak. */
static bool reply_allowed(const uint8_t id[4]) {
    int64_t now = esp_timer_get_time();

    if (now - s_budget_window_us >= 60 * 1000000LL) {
        s_budget_window_us = now;
        s_budget = MN_BOT_MAX_PER_MIN;
        s_budget_warned = false;
    }
    if (s_budget <= 0) {
        if (!s_budget_warned) {
            s_budget_warned = true;
            mn_emit_event("!WARN bot-budget-exhausted (%d/min) holding off",
                          MN_BOT_MAX_PER_MIN);
        }
        return false;
    }

    int slot = -1, oldest = 0;
    for (int i = 0; i < MN_BOT_COOLDOWN_SLOTS; i++) {
        if (s_cool[i].used && !memcmp(s_cool[i].id, id, 4)) {
            if (now - s_cool[i].last_us < MN_BOT_PEER_COOLDOWN_US) return false;
            slot = i;
            break;
        }
        if (!s_cool[i].used && slot < 0) slot = i;
        if (s_cool[i].last_us < s_cool[oldest].last_us) oldest = i;
    }
    if (slot < 0) slot = oldest;            /* evict the stalest tracked peer */

    s_cool[slot].used = true;
    memcpy(s_cool[slot].id, id, 4);
    s_cool[slot].last_us = now;
    s_budget--;
    return true;
}

/* ---- bot 0: proof of life ----
 * Deliberately not a conversation: it answers one line so a human watching the
 * app feed, or a soak harness parsing !CHAT, can see that a node is awake,
 * decrypting on the right channel, and able to transmit. */
static void bot_proofoflife(const uint8_t sender_id[4], const char *sender_name,
                            const char *text, size_t len) {
    (void)sender_name; (void)text; (void)len;
    if (!reply_allowed(sender_id)) return;

    char when[32];
    mn_time_str(when, sizeof(when));

    char reply[96];
    snprintf(reply, sizeof(reply), "Hey %02x%02x%02x%02x, it's %s.",
             sender_id[0], sender_id[1], sender_id[2], sender_id[3], when);
    mn_chat_automated(reply, strlen(reply));
}

/* ---- registry ----
 * Ids are stable and gap-tolerant: a bot may be reserved here (handler NULL)
 * before it exists, so `botmode` documents the roadmap and the numbering never
 * shifts under a saved script. Keep this table sorted by id — mn_bot_list()
 * prints it in order and makes no attempt to sort at runtime. */
typedef void (*mn_bot_fn)(const uint8_t sender_id[4], const char *sender_name,
                          const char *text, size_t len);

typedef struct {
    int         id;
    const char *name;
    const char *desc;
    mn_bot_fn   on_chat;      /* NULL = reserved, selecting it is an error */
} mn_bot_t;

static const mn_bot_t s_bots[] = {
    { 0, "proofoflife", "one-line 'Hey <id>, it's <time>' reply",
      bot_proofoflife },
    { 1, "eliza", "gen-1 Weizenbaum DOCTOR (not built — see docs/BOT-MODE.md)",
      NULL },
};
#define MN_BOT_COUNT ((int)(sizeof(s_bots) / sizeof(s_bots[0])))

static int s_active = -1;      /* index into s_bots, -1 = off */

static int bot_index(int id) {
    for (int i = 0; i < MN_BOT_COUNT; i++)
        if (s_bots[i].id == id) return i;
    return -1;
}

int mn_bot_set(int id) {
    if (id < 0) {                            /* any negative value = off */
        s_active = -1;
        mn_emit_event("# bot off");
        return 0;
    }
    int i = bot_index(id);
    if (i < 0 || !s_bots[i].on_chat) return -1;
    s_active = i;
    /* Start each arming with a clean slate, so a cooldown left over from an
     * earlier session cannot swallow the first reply of this one. */
    memset(s_cool, 0, sizeof(s_cool));
    s_budget_window_us = 0;
    mn_emit_event("# bot on id=%d name=%s", s_bots[i].id, s_bots[i].name);
    return 0;
}

int mn_bot_get(void) { return (s_active < 0) ? -1 : s_bots[s_active].id; }

void mn_bot_list(void) {
    for (int i = 0; i < MN_BOT_COUNT; i++)
        mn_emit_event("# bot %d %s%s — %s", s_bots[i].id, s_bots[i].name,
                      s_bots[i].on_chat ? "" : " (reserved)", s_bots[i].desc);
    if (s_active < 0) mn_emit_event("# bot active=none");
    else mn_emit_event("# bot active=%d %s", s_bots[s_active].id,
                       s_bots[s_active].name);
}

void mn_bot_on_chat(const uint8_t sender_id[4], const char *sender_name,
                    const char *text, size_t len,
                    uint8_t flags, bool was_multicast) {
    (void)was_multicast;
    if (s_active < 0) return;
    if (flags & MN_F_AUTOMATED) return;                  /* guard 1: no volley */
    if (!memcmp(sender_id, mn_device_id(), 4)) return;   /* guard 2: not us    */
    s_bots[s_active].on_chat(sender_id, sender_name, text, len);
}

#endif /* MN_ENABLE_BOTS */
