# NEKOBOT — technical design spec

*A bonsai-model cat chatbot that drives one Hanasu bench node over UART,
written in Pop-11. First landed 2026-08-15; validated the same day in a
5-minute proof session and a ~6-hour conversation run alongside a passing
gated soak.*

Code: `tools/catbot.p` (the bot), `tools/uartpipe.py` (serial bridge).
Transcripts: `tools/logs/catbot-*.log`.

---

## 1. Purpose and goals

The soak harness (`tools/soak.py`) proves the mesh moves *mechanical* traffic
losslessly. Nekobot adds the missing complement: **organic, conversational,
irregular traffic** — replies triggered by other nodes' messages, spontaneous
initiations on a jittered cadence, and mood-dependent content — while
producing a transcript a human enjoys reading. Design goals, in order:

1. **Exercise the mesh like a chat app would**, not like a load generator.
2. **Host-driven, zero firmware change** — the node is the bot's mouthpiece;
   any stock 0.6.0-eg node can be a cat.
3. **Terse by construction** — every utterance must fit the 62-byte
   single-frame class, the only multicast class measured lossless (E-B
   throughput study: 62 B single-frame = 0.00% loss; 496 B fragmented ≈ 7%
   loss even on a quiet channel).
4. **Storm-proof** — a bot must be unable to amplify (the E-E hook-loop
   incident is the cautionary tale).
5. **Legible transcripts** — the log shows both the cat utterance and the
   pre-DSL template, so a reviewer can audit what the model "meant".

Non-goals: natural-language understanding, LLM integration, firmware
residency (that's `MN_ENABLE_BOTS` / `BOTMODE`, a separate compile-time
feature — see `docs/BOT-MODE.md`).

## 2. Architecture

```
mesh ⇄ node "neko" ⇄ USB-serial-JTAG ⇄ uartpipe.py ⇄ rx/tx FIFOs ⇄ catbot.p (Pop-11)
                                        (owns port)                  ├─ layer 3: driver
                                                                     ├─ layer 2: catify DSL
                                                                     └─ layer 1: bonsai model
```

### 2.1 uartpipe.py — the serial bridge

A deliberately dumb byte pump. It exists because of a hardware discovery made
during bring-up: **a bare `open(2)` of the C6's USB-serial-JTAG device
receives nothing.** The host must assert DTR (pyserial does at open; a plain
open does not), otherwise the firmware's drop-on-stall TX guard — added in
the E-F serial-TX-stall fix — discards every byte queued for a host that
isn't signalling readiness. Rather than teach Pop-11 (or any future host
language) termios + ioctl modem-line control, the bridge owns the port the
proven pyserial way (the same open `soak.py` holds for hours) and
re-presents it as two FIFOs any process reads and writes like ordinary
files.

Properties:

- Port opened **once** and held (reopening resets the C6).
- Either FIFO peer may disconnect and return; the bridge re-blocks on FIFO
  open and resumes. The **serial port dying is fatal** (exit 1) so a
  supervisor notices.
- One process per serial port, no exceptions. Running `hcp.py` against a
  bridge-held port kills the bridge with pyserial's *"device reports
  readiness to read but returned no data (multiple access on port?)"* —
  this was learned the hard way mid-run.

Pop-11 reads the rx FIFO non-blockingly via a `FIONREAD` ioctl
(`sys_io_control`, macOS request `0x4004667F`) because `sys_input_waiting`
returns false for FIFOs.

### 2.2 Layer 1 — the bonsai model

A hand-pruned generative model, tiny on purpose. Two state variables and two
tables:

- **Intent classifier** — keyword match over the lowercased inbound text,
  first hit wins: `ping` (contains `soak#`) → `food` → `nap` → `play` →
  `greet` → `question` (contains `?`) → `free` (fallback).
- **Reply table** — 3–5 terse-English templates per intent
  (`'feed me now'`, `'five more naps'`, `'ask the tail'` …).
- **Mood** — one of `purr | hongry | slepy | zoomies`, re-rolled every
  5–10 minutes. Mood selects the initiation repertoire and the DSL's tail
  particles, so the bot's voice drifts audibly over hours.
- **Initiation table** — per-mood templates fired on a jittered 60–150 s
  timer when the throttles allow.

Randomness is a session-seeded LCG (`cb_rand`), not `random()` — cheap,
portable, and reproducible enough that a transcript anomaly can be reasoned
about.

Why a bonsai model and not an LLM: deterministic and auditable (every
utterance traces to a template visible in the log), sub-millisecond on the
conversation path, works fully offline, and — decisively — **terse by
construction**. A generative model would need its output policed down to
30 characters; a template table simply never exceeds it.

### 2.3 Layer 2 — the catify DSL

`catify(terse_english) -> cat` compiles templates to wire text through four
stages:

1. **Lowercase + tokenize** (space/tab split).
2. **Article drop** — `the a an is am to of` are deleted. Cats have no
   articles.
3. **Lexicon rewrite** — ~40 pairs, longest-standing register of the cat
   dialect: `hello→mrrp`, `yes→nya`, `no→hss`, `food→fud`, `hungry→hongry`,
   `sleep→nap`, `you→u`, `have→haz`, `little→smol`, `big→chonk`, …
4. **Mood tail particle** (60% probability) — `purr` moods append
   `nya | prr | =^.^=`, `slepy` appends `zzz | mrr | ...`, `zoomies`
   appends `!! | nyoom | mao!`, then a **hard truncate at 30 chars**.

The 30-char cap plus `CHAT ` framing and envelope overhead keeps every frame
in the 62-byte lossless class (§1 goal 3). Examples from the validation run:

```
hello friend do you have food  →  mrrp frend do u haz fud prr
i am very sleepy now           →  i so slepy nao =^.^=
what are you playing           →  wat r u playing nya
```

The DSL is a data table, not code: extending the dialect is adding a pair to
`cb_lexicon` or a particle list, live, in the running session
(`popsession send`), with no restart.

### 2.4 Layer 3 — the driver

- **Line pump**: drain FIFO → split lines → dispatch. `!CHAT <chan> <idhex>
  <name> <text>` lines feed the model; `+OK state=…` feeds
  self-identification; heartbeats/`# `/`@` lines are dropped as routine.
- **Throttles** — deliberately mirroring `magnet_bot.c`'s firmware-bot
  policy so host bots and firmware bots are interchangeable citizens:
  - 20 s minimum gap between own sends,
  - 45 s per-sender cooldown,
  - global budget 6 sends/minute,
  - soak pings answered with probability 1/4 (cats mostly nap through
    telemetry).
  These sit *below* the firmware's own §4.9 token bucket (burst 8, 10/s), so
  the bot can never even reach the node's rate limiter, let alone the
  E-E amplification ceiling.
- **Self-filter**: own name discarded on receipt; a bot must never converse
  with itself.
- **Transcript**: every heard/spoken chat logged as
  `HH:MM:SS name: text   [template]` — the `[template]` suffix on own lines
  is the audit trail from wire text back to model intent.

### 2.5 Session model

The bot lives in a persistent Pop-11 session (`popsession --name catbot`);
procedures compile once (~6 ms for the whole file) and survive across runs.
Checkpoint at `~/.cache/pop11-skill/hanasu-catbot-v1.psv` restores the
compiled bot in ~8 ms — FIFO/log device handles go stale across restore by
design and are reopened. Long runs are a single
`popsession send -c 'catbot_run(21600);'` in the background; a mishap
(e.g. bridge death) kills the chunk but **the session and its state
survive**, so recovery is: restart bridge, reopen FIFOs, `catbot_run` again.
Both recoveries during the validation day took under a minute.

## 3. Validation results (2026-08-15)

| Session | Duration | Outcome |
|---|---|---|
| Proof session | 5 min | Replied to greet/play/sleep prompts from 2 peers, 2 self-initiations, 1 correctly-throttled ignore |
| Long run | ~6 h across 3 segments | 646 chat lines, 303 spoken by neko; ran **concurrently with the 6-h gated soak, which passed** (668/668 delivery, heap +0 B, rx err 0) |

The concurrent GATE PASS is the load-bearing result: conversational bot
traffic added to chat-rate soak traffic cost the mesh nothing measurable.

The long run was segmented by two external events, both instructive:

1. **Hub power loss at 5 h 34 m** — the bench USB hub cut power ≈5½ h into
   sustained use (second occurrence; see bench report). Nodes rebooted
   (reset-reason=POWERON), NVS kept the bot node's name and channel, the
   mesh re-formed in seconds, and the bot resumed after a bridge restart.
2. **Probe collision** — an `hcp.py` status sweep against the bridge-held
   port killed the bridge (§2.1). Now a documented ground rule.

## 4. Findings & design experience

**What the bonsai bet paid.** The model is ~60 lines of tables and it
produced six hours of transcript that reads as *characterful* rather than
random. The mechanism is the separation of layers: the model deals only in
readable terse English, the DSL owns the register, and the transcript shows
both. Reviewing a night of output takes minutes because every line carries
its own explanation.

**Constraints read as personality.** The most-liked moment of the proof
session was a *throttle*: probe asked "neko do you want food" 1 s after neko
had spoken, the min-gap suppressed the reply, and the log reads as a cat
ignoring you. Rate limits, nap-through-pings probability, mood drift — every
anti-storm mechanism doubles as characterization. This is worth keeping as a
design principle: **make the safety envelope part of the persona.**

**The wire constraint improved the writing.** 30 chars forces the terse
register ("bowl status empty", "head boop for all") that makes the cat voice
work. A looser budget would have produced worse transcripts *and* worse
frames.

**Serial is the hard part, and it's now solved once.** Of the day's
engineering time, the model+DSL took perhaps a fifth; the rest went to the
UART path (DTR gate discovery, FIFO bridge, FIONREAD, two bridge deaths).
That cost is now sunk into `uartpipe.py` + documented traps — but the
lesson generalizes: **the next driver should not own serial at all.** The
plan of record is to re-point layer 3 at the chat-bench web bridge
(`tools/chat-bench/bridge.py`, PR #96), which owns all ports and speaks
HTTP/SSE — making port collisions structurally impossible and giving every
utterance a measured end-to-end latency for free.

**Pop-11 held up.** Incremental compilation made the edit loop instant
(redefine one procedure mid-session), the whole-heap checkpoint made
recovery trivial, and a native-code table-driven bot is comfortably
overpowered for a 0.2 s tick. Gaps encountered and their idioms: no JSON
(bridge future: pipe `curl -sN | jq --unbuffered` through
`sys_obey_linerep`), `sys_input_waiting` false on FIFOs (FIONREAD ioctl),
device writes need `sysflush`.

## 5. Future work

- **Bridge-native driver** (plan of record): layer 3 over chat-bench
  HTTP/SSE; browser panes show the conversation live; latency stamps in the
  transcript.
- **LED mood** (lighting proposal): map `cb_mood` to the NanoC6's on-board
  RGB via a new `LED` verb — the cat's mood becomes visible on the bench.
- **Photo turns**: neko posts a Type 6 image mid-conversation — exercises
  the E-H transfer path under chat load.
- **Second persona**: a firmware `BOTMODE` bot (or a second host bot with a
  different lexicon) as a conversation partner, so long runs are dialogues
  rather than monologues-with-telemetry. The per-sender cooldowns on both
  ends bound the loop by construction.
- **Name resolution nit**: peers that haven't announced since the bot's boot
  show as `-` in `!CHAT`; a `RECENT`-style name refresh or an announce
  re-request would fix transcript attribution after mid-run reboots.
