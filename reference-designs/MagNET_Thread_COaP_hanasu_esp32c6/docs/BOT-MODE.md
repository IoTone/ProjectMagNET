# Bot mode — opt-in auto-responders

**Status: HARDWARE-VALIDATED 2026-08-04** on the 4-node bench, two nodes
carrying the bots build and two without — see *Bench results* below.

Bot mode arms one auto-responder on a node so it answers inbound chat by
itself. It exists for proof-of-life: watching an app feed or a soak log and
seeing a node reply tells you in one line that it is awake, attached, holding
the right channel key, and able to transmit. It is a **test mode**, not a
product feature, and it is compiled out of every fielded image.

## Using it

Both front-ends drive the same `mn_bot_*` calls, the split §12.3 already uses
for the `mn-*` vocabulary.

| Intent | Forth | HCP |
| --- | --- | --- |
| List the bots (ascending by id) | `botmode` | `BOTMODE` |
| Arm bot 0 | `0 botmode` | `BOTMODE 0` |
| Turn it off | `-1 botmode` | `BOTMODE OFF` |

```
> BOTMODE
# bot 0 proofoflife — one-line 'Hey <id>, it's <time>' reply
# bot 1 eliza (reserved) — gen-1 Weizenbaum DOCTOR (not built — see docs/BOT-MODE.md)
# bot active=none
+OK
> BOTMODE 0
# bot on id=0 name=proofoflife
+OK
```

Bot 0 then answers any human chat once per sender per 30 s:

```
!CHAT magnet 2ca44570 xray1 anyone home?
!CHAT magnet e1256131 probe Hey 2ca44570, it's 14:32:07.
```

Bot mode is **per-boot state on purpose** — it is not persisted to NVS. A node
that resumes talking to the channel by itself after a power cycle is a surprise
nobody wants at 3 a.m.

### `botmode` with no argument

`botmode` is a Forth colon definition, not another FFI primitive, for one
reason: an FFI word cannot tell an empty stack from a pushed `0`. This engine's
`pop()` returns 0 on underflow, so a bare `botmode` would read as `0 botmode`
and arm bot 0 — the exact opposite of "with no argument, list the bots".
`depth` sees the caller's stack and disambiguates:

```forth
: botmode  depth 0> if mn-bot! else mn-bots then ;
```

It is registered at bringup from `mn_register_forth_vocab()` and costs about
10 cells of the 4096-cell code space. The underlying primitives `mn-bot!`
( n -- ) and `mn-bots` ( -- ) remain available if you want them directly.

The usual Forth caveat applies: `botmode` alone consumes a leftover stack item
if you left one there. `.s` first if you are unsure.

## The clock

Bot 0 wants to say what time it is, and **a Hanasu mesh has no wall clock**.
There is no border router, so no SNTP; `esp_timer` only ever yields uptime.
This grew into a mesh-wide shared clock with its own document —
**`MESH-TIME.md`** — but the short version is that a host seeds one node and
that node multicasts to the channel:

```
python tools/hcp.py synctime   # seeds one node from this host, pushes to mesh
TIME                           # → +OK clock=14:32:07 tz=+540 stratum=0 src=host
```

Forth: `1785000000 mn-time!` (UTC only — a cell is 32 bits on the C6, so this
form is good until 2038; the HCP verb takes 64 bits). `mn-now` prints the full
state, `mn-time-sync` pulls from the mesh.

Until it is set, `mn_time_str()` reports uptime — `up 3h07m` — and says
`clock=unset` when asked directly. It never invents a date.

This deliberately does **not** use `settimeofday` + `localtime_r` + `strftime`.
That path measured **+8.8 KB of flash** in newlib's time formatting and TZ
parsing to print eight characters. The clock is anchored to `esp_timer` and the
time of day falls out of three integer divisions instead, for +1.2 KB. The
trade is that only the time of day is available — no date, no DST rules —
which is all a bot line or a bench log stamp ever wanted.

## Build option and measured cost

Bot mode lives behind `MN_ENABLE_BOTS`, defaulting to 0. It is its own env
rather than a flag on the existing ones, so the hardware-validated builds stay
bit-identical to what was soaked:

```
pio run -e esp32c6_ble_resident_bots -t upload --upload-port /dev/cu.usbmodemXXXXX
```

Measured on `esp32c6_ble_resident` (ESP32-C6, 2.75 MB app partition, 320 KB SRAM):

| Build | Flash | ΔFlash | RAM | ΔRAM |
| --- | ---: | ---: | ---: | ---: |
| Baseline (before this work) | 1,094,695 | — | 130,736 | — |
| `+ TIME` + mesh clock (all builds) | 1,097,485 | +2,790 | 131,356 | +620 |
| `+ MN_ENABLE_BOTS=1` | 1,099,209 | +1,724 | 131,584 | +228 |

**Total +4,514 B flash (0.16 % of the partition) and +848 B RAM**, of which only
+1,724 B / +228 B is the bot machinery itself — the clock is unguarded because
a shared mesh time is useful on its own (see `MESH-TIME.md`). Flash sits at
39.9 % and RAM at 40.2 %, so this is not close to a constraint. The
`Dial DRAM budget` warning
that applies elsewhere in MagNET does not bite here: the C6 build has ~197 KB
of SRAM headroom.

## Why the bots are C, not Forth

The embedded ESPIDFORTH engine has, in full: arithmetic, stack ops (`depth`,
`pick`, `>r`/`r>`), comparisons, `variable`/`constant`, `s"`, `if`/`else`/
`then`, `begin`/`until`/`again`/`while`/`repeat`, `do`/`loop`/`+loop`, memory
access (`here`, `allot`, `!`, `@`, `c!`, `c@`), `type`, and colon definitions.

It has **no** `create`/`does>` (no data structures), **no** `'`/`execute` (no
dispatch tables), **no** `,`/`c,` (no compile-time data), and **no** string
words at all beyond MagNET's own `str=` FFI — no `compare`, `search`, `move`,
`count`, `fill`. Compiled code for every definition shares one 4096-cell
(16 KB) array, with `MAX_WORDS` 512 and a 256-byte input line.

Anything that takes incoming text apart therefore belongs in C. Forth stays the
control surface. That is not a workaround — it is the same division the whole
`mn-*` vocabulary already uses.

## The loop problem

This is the entire design. A responder that answers every chat will, on a mesh
where two nodes both have one armed, volley forever: A's reply is B's stimulus
and B's reply is A's. The E-E bench test already found this with Forth hooks
and capped them at 5/s — but 5 messages a second sustained still ruins a
channel. Bot mode adds four independent guards, cheapest first:

1. **`MN_F_AUTOMATED` (envelope flag bit 6).** Bot replies are marked; bots
   never answer a marked frame. This deterministically breaks bot↔bot. Bits 6
   and 7 were free and the decoder copies the flag byte verbatim, so older
   firmware ignores it — the change is wire-compatible.
2. **Never answer ourselves** — our own device id, i.e. multicast loopback.
3. **Per-peer cooldown**, 30 s. One reply to a given sender per window, so a
   chatty human cannot be spammed.
4. **Global budget**, 6 replies/minute across all peers, warning once via
   `!WARN bot-budget-exhausted`. The backstop for "twelve peers all say hi at
   once".

Guards 2–4 exist because guard 1 trusts the sender, and a node running older or
foreign firmware cannot be trusted to set a flag. Replies also pay the normal
§4.9 token bucket — a bot is a host like any other.

## Bench results (2026-08-04)

Two nodes flashed with `esp32c6_ble_resident_bots` (xray1, probe) and two with
plain `esp32c6_ble_resident` (sdk-b, xray2), which also proves a non-bot build
ignores `MN_F_AUTOMATED` harmlessly.

| # | Check | Result |
| --- | --- | --- |
| 1 | `BOTMODE` lists bots ascending, marks `eliza` reserved | PASS |
| 2 | Non-bot build answers `-ERR E_UNSUPPORTED` rather than going quiet | PASS |
| 3 | `BOTMODE 0` arms, emits `# bot on id=0` | PASS |
| 4 | Bot replies to a human chat | PASS |
| 5 | Reply carries short id + real clock time | PASS — `Hey 46a359bf, it's 17:04:30.` |
| 6 | 30 s per-peer cooldown suppresses an immediate second chat | PASS |
| 7 | **Both bots armed, one stimulus → each answers once, no volley** | PASS |
| 8 | `BOTMODE OFF` silences them | PASS |

Check 7 is the one that mattered. Both bots replied 0.1 s after the stimulus
and neither answered the other; 40 s of watching produced exactly two distinct
replies (6 `!CHAT` observations, because each reply is seen by the other three
nodes). `MN_F_AUTOMATED` survives the round trip.

Watch out when counting replies in a harness: `!CHAT` lines embed the
**receiving** node's view of the sender's display name, so one frame seen by
two nodes that disagree about a name looks like two frames. Dedup on sender id
plus text, not on the whole line.

**Still unverified:** behaviour under sustained multi-peer load (the 6/min
global budget has not been driven to exhaustion on hardware).

## Bot 1 — an ELIZA, and what it would actually cost

Bot id 1 is reserved in the registry (selecting it returns `E_NO_BOT` today) so
the numbering never shifts under a saved script.

### What a gen-1 ELIZA actually is

Weizenbaum's 1966 DOCTOR script is not a parser and not a state machine. It is:

- a **keyword list** (~40 words: *sorry, remember, if, dream, mother, computer,
  am, are, your, was, I, you, yes, no, my, can, because, why, everyone,
  always, like*), each with a **precedence rank** — the highest-ranked keyword
  present in the sentence wins;
- per keyword, **decomposition rules** with wildcards, e.g. `(0 I ARE 0)` where
  `0` matches any run of words, capturing the fragments;
- **reassembly templates** cycled round-robin so a repeated keyword does not
  give a repeated answer, e.g. `WHY DO YOU THINK YOU ARE (4)`;
- a **pre/post substitution list** applied to captured fragments — *I→you,
  my→your, me→you, am→are, myself→yourself* — which is what makes the echo
  sound like a question rather than a parrot;
- a **memory queue**: on certain keywords, stash the transformed input and
  replay it later when nothing matches ("Earlier you said your mother…");
- a **NONE list** of fallbacks ("Please go on", "I see").

The illusion comes almost entirely from the pronoun swap plus template
cycling. Skip those two and you have a parrot, not an ELIZA.

### Three ways to build it

**Option A — ELIZA in C, behind this same bot registry. ~1–2 days.**
Tokenize and case-fold (~40 lines), a static keyword/rank table (~20 keywords
is plenty for a demo), fragment capture after the matched keyword, a ~15-pair
pronoun swap table, cycled reassembly templates, fallbacks and the memory
queue. Roughly 300 lines and 2–3 KB of `.rodata` tables. Estimate **+4–6 KB
flash, ~0.5 KB RAM**, all of it inside `MN_ENABLE_BOTS` so the fielded image is
untouched. Full wildcard decomposition (rather than "keyword + rest of
sentence") is the one part worth deciding up front; it adds ~80 lines and is
what separates a convincing DOCTOR from a Markov-ish toy.

**Option B — ELIZA in Forth. ~4–6 days, high risk. Not recommended for the
chatbot's sake.**
It cannot be written as a script today. You would first add roughly 10–14
primitives to the engine — `count`, `compare`, `search`, `move`, `fill`, `c,`,
`,`, `create`, `'`, `execute`, `>body`, probably `evaluate` — about 250 lines
of C. Two costs beyond the time: those land in
`ESPIDFORTH/components/forth/forth_core.cpp`, which this project reaches
through a **symlink shared with `MagNET_M5DialFiddlerCrab`**, so the change
hits another project and diverges from upstream ESP32forth. And the script
itself is tight: every `s"` literal is stored inline in the shared 16 KB code
array, so ~20 keywords × ~3 templates × ~40 chars is already ~2.4 KB before the
matcher words, against `MAX_WORDS` 512. Debugging string code in a Forth with
no `dump` and no error recovery is slow work.

The honest framing: **the value of Option B is the string primitives, not the
chatbot.** If scriptable text handling is wanted for other reasons, that is the
justification — and then ELIZA is a nice proof of it. Do not buy the primitives
to get a chatbot.

**Option C — hybrid. ~1.5 days.**
C provides the sharp edges as FFI words — `mn-kw-find ( c-addr u -- rank idx )`,
`mn-swap-pronouns`, `mn-say ( idx -- )` — and the keyword table, patterns and
policy live in Forth, editable at runtime through `SCRIPT SET` with no reflash.
This is the option that actually advances the §12.3 "scriptable node" story
rather than just adding a demo, and it is the one to pick if bots are meant to
be a platform rather than a test fixture.

**Recommendation:** Option A if bot 1 is a demo, Option C if scriptable
personalities are a goal. Either way the cost stays behind `MN_ENABLE_BOTS`.

## Files

| File | What changed |
| --- | --- |
| `components/magnet/magnet_bot.c` / `include/magnet_bot.h` | new — registry, guards, bot 0 |
| `include/magnet_envelope.h` | new `MN_F_AUTOMATED` flag (bit 6) |
| `magnet_core.c` | `extra_flags` on `send_frame_ex`, `mn_chat_automated`, the clock, RX hookup |
| `magnet_forth.c` | `mn-bot!`, `mn-bots`, `mn-time!`, `mn-now`, `botmode` bootstrap |
| `magnet_link.c` | `BOTMODE` and `TIME` verbs, CAPS/HELP |
| `platformio.ini` | new env `esp32c6_ble_resident_bots` |
