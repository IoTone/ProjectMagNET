/*
 * magnet_bot.h — MagNET Hanasu bot mode (test/demo auto-responders).
 *
 * A "bot" is a C responder that may reply to inbound chat. Exactly one bot is
 * active at a time, selected by numeric id; -1 (or any negative) = off, which
 * is the state at every boot (bot mode is deliberately NOT persisted — a node
 * that starts answering by itself after a power cycle is a support call).
 *
 * Control surface, both front-ends onto the same mn_bot_* calls (§12.3):
 *   Forth: `0 botmode`  select bot 0     `-1 botmode`  off     `botmode`  list
 *   HCP:   `BOTMODE 0`                   `BOTMODE OFF`         `BOTMODE`
 *
 * Compiled out entirely unless MN_ENABLE_BOTS=1 (env esp32c6_ble_resident_bots)
 * — the stubs below cost nothing when it is off. See docs/BOT-MODE.md.
 */
#ifndef MAGNET_BOT_H
#define MAGNET_BOT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef MN_ENABLE_BOTS
#define MN_ENABLE_BOTS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if MN_ENABLE_BOTS

/* Select the active bot. id < 0 turns bot mode off. 0 = ok, -1 = no such bot. */
int  mn_bot_set(int id);
/* Active bot id, or -1 when off. */
int  mn_bot_get(void);
/* Emit one '# bot <id> <name> <desc>' line per bot, ascending by id, plus a
 * '# bot active=…' summary. */
void mn_bot_list(void);

/* Offer one accepted inbound chat frame to the active bot. Called from the
 * PUMP TASK only (same context as the Forth hooks — never OT/lwIP callback
 * context, because a reply sends, and sending takes the OT lock).
 *
 * The caller has already decrypted, deduped and unwrapped the frame; `flags`
 * is the envelope's flag byte so the bot can refuse to answer another bot. */
void mn_bot_on_chat(const uint8_t sender_id[4], const char *sender_name,
                    const char *text, size_t len,
                    uint8_t flags, bool was_multicast);

#else  /* bots compiled out — every call folds to nothing */

static inline int  mn_bot_set(int id) { (void)id; return -1; }
static inline int  mn_bot_get(void)   { return -1; }
static inline void mn_bot_list(void)  { }
static inline void mn_bot_on_chat(const uint8_t sender_id[4],
                                  const char *sender_name, const char *text,
                                  size_t len, uint8_t flags, bool was_multicast)
{ (void)sender_id; (void)sender_name; (void)text; (void)len; (void)flags;
  (void)was_multicast; }

#endif /* MN_ENABLE_BOTS */

#ifdef __cplusplus
}
#endif

#endif /* MAGNET_BOT_H */
