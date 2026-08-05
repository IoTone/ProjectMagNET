# Mesh time — a shared clock without NTP

**Status: HARDWARE-VALIDATED 2026-08-04**, 4-node bench (xray1, sdk-b, xray2,
probe), fw built from this tree. In all builds, not gated by a flag.

A Hanasu mesh has no border router, so there is no SNTP and `esp_timer` only
ever yields uptime. But a shared clock is worth having — log correlation across
nodes, timestamped chat in the app, and anything a bot or script wants to say
about "now". The host has a real clock; this pushes it into the mesh.

## The shape of it

One host seeds **one** node over HCP. That node becomes **stratum 0** and
multicasts the clock to the channel. Every other node adopts it at
**stratum + 1**.

```
  host (Mac/phone)          stratum 0                stratum 1
  ───────────────  TIME SET ─────────  ns0/cmd0x04  ──────────  …
      real clock              anchor      multicast    followers
```

```
$ python tools/hcp.py synctime           # seeds the first node it finds
/dev/cu.usbmodem11201   TIME SET 1785883701 -420
+OK clock=15:48:21 tz=-420 stratum=0 src=host age=0s (pushed to mesh)
```

Everyone else, within a second:

```
# time adopted from 2ca44570 (clock=15:48:21 tz=-420 stratum=1 src=2ca44570 age=0s)
```

## Verbs

| Intent | HCP | Forth |
| --- | --- | --- |
| Show clock, stratum, source, age | `TIME` | `mn-now` |
| Seed from host (and push to mesh) | `TIME SET <epoch> [<tz-min>]` | `<epoch> mn-time!` |
| Re-announce our clock | `TIME PUSH` | `mn-time-push` |
| Ask the mesh for the time | `TIME SYNC` | `mn-time-sync` |

`tz-min` is minutes east of UTC — JST `540`, PDT `-420`, UTC `0`. Only the time
of day is kept, not the date: see *Why not `settimeofday`* below.
`python tools/hcp.py synctime [<port>]` computes both from the host and sends
them, which is the intended everyday path.

## Wire format

System M2M command (Type 1, ns `0x00`), joining `0x02` announce-name and
`0x03` rotate:

**`0x04` — time announce**, 11 params:

| Bytes | Field |
| --- | --- |
| 0–7 | epoch seconds, int64 big-endian, UTC |
| 8–9 | tz offset minutes, int16 big-endian |
| 10 | stratum **of the sender** (receivers adopt stratum + 1) |

**`0x05` — time request**, no params.

## The four rules that make it stable

**1. Receivers never re-broadcast.** Thread's MPL already floods realm-local
multicast across the whole mesh, so forwarding buys no reach and costs a storm.
One announce reaches everyone.

**2. A better stratum always wins; a worse one is always ignored.** At *equal*
stratum a node accepts a refresh from the source it already follows, and
otherwise breaks the tie on lowest sender id. That tie-break is load-bearing:
without a deterministic rule, two equal peers re-adopt each other's clock
forever, each round trip folding the announce latency in as drift. Anything at
stratum ≥ 4 is refused outright, which bounds how far from a real clock the
mesh will propagate.

**3. Requests get exactly one answer, whatever the mesh size.** A `TIME SYNC`
multicast makes every node that *has* a clock schedule a jittered reply — and
hearing any announce cancels a pending one. The delay is biased by stratum
(stratum 0 waits ~50–400 ms, stratum 1 ~450–800 ms, …), so the best clock in
earshot answers first and everyone else stands down. This is the same
Trickle-style suppression §11.6 already uses for the name announce.

**4. Convergence is automatic, not manual.** The stratum-0 node re-announces
every 15 minutes, so late joiners and rebooted nodes catch up without anyone
asking. A node that reaches READY with no clock sends one request; a node that
reaches READY *as* stratum 0 pushes instead. Nodes that already have a clock
stay silent, so a partition heal does not become a request storm.

## Trust model

These frames are channel-encrypted like everything else, so **anyone who can
send chat can set the mesh clock**. That is deliberate: it is the same boundary
chat already has, and nothing security-critical depends on the clock — the
replay defence in §11.1 is the monotonic counter, not a timestamp.

If that ever changes, this becomes an `ADMIN|SIGNED` frame like system/rotate,
verified against the admin allow-list. Carrying a stratum in the payload rather
than a bare timestamp is what leaves that door open.

## Accuracy, and what this is not

Roughly **±1 second**. There is no round-trip delay compensation, no drift
discipline, and no date — this is not NTP and should not be used as if it
were. Frame latency is folded in as-is at each hop, which is why the stratum
cap matters. The C6 crystal drifts on the order of a second a day; the 15
minute refresh keeps followers well inside that.

If sub-second accuracy is ever needed, the honest answer is to measure the
round trip (`TIME SYNC` timestamped on both ends) and halve it, or to use
Thread's own network time if a border router appears. Neither is worth doing
for log stamps.

## Two seeded nodes

Seed one. If you seed two, both stay stratum 0 (each ignores the other, since
a heard announce would put it at stratum 1, which is worse than 0), both keep
announcing on their own 15 minute cadence, and stratum-1 followers converge on
whichever anchor has the **lower device id** by the tie-break rule. It is
deterministic and it does not oscillate — but the two anchors can still
disagree with each other, so it is not a configuration to want.
`hcp.py synctime` seeds exactly one node for this reason.

## Bench results (2026-08-04, 4 nodes)

| # | Check | Result |
| --- | --- | --- |
| 1 | All four boot with `clock=unset` | PASS |
| 2 | Seeded node becomes `stratum=0 src=host` | PASS |
| 3 | All three followers adopt | PASS |
| 4 | Followers land at exactly stratum 1 | PASS |
| 5 | Node clocks agree with each other | PASS — 1 s spread |
| 6 | Mesh clock matches the host | PASS — −1 s on every node |
| 7 | `TIME SYNC` draws exactly **one** answer, not four | PASS — one sender |
| 8 | Rebooted follower re-adopts with no host involvement | PASS — 1.3 s after boot |

Seeding to full mesh convergence took under a second. The rebooted node logged
`# time adopted from 2ca44570 (clock=17:08:29 tz=-420 stratum=1 …)` 1.3 s after
coming up, which exercises the request-on-READY path rather than the 15 minute
refresh.

Two cautions for anyone writing a harness against this. Comparing a node's
`clock=` to the host requires timestamping the *arrival of the reply* — read
the host clock after a settle window and every node looks seconds slow, which
is the harness lagging, not the mesh. And `!CHAT` lines embed the **receiving**
node's view of the sender's display name, so the same frame observed on two
nodes that disagree about a name is not two frames; dedup on sender id plus
text.

**The 15 minute stratum-0 refresh is verified** (2026-08-04, seen incidentally
while testing the app): all three followers re-adopted from the anchor
simultaneously, ~15 min after their previous adopt, with no request involved.

**App-side seeding verified on hardware** — SH-53D over BLE to the `probe`
companion. Twice, from a mesh where `xray1` was the anchor and `probe` was a
stratum-1 follower:

| | probe before | probe after app connect |
| --- | --- | --- |
| first connect | `stratum=1 src=2ca44570` | `stratum=0 src=host age=103s` |
| after reboot + reconnect | `stratum=1 src=2ca44570 age=564s` | `stratum=0 src=host age=42s` |

`src=host` can only come from an HCP `TIME SET`, and the phone was the only
host attached to probe, so this is unambiguous. Rebooting the old anchor then
left the mesh cleanly anchored on the app-seeded companion — `xray1` came back
and adopted from `e1256131` at stratum 1, which is the shape a deployed mesh
should hold.

One nuance worth knowing when testing: the Live-mesh screen only auto-connects
from `idle`, so after a dropped link it sits in `Connection lost` until the
user taps reconnect. The seed rides on *successful connects*, manual ones
included — it just is not automatic while the app is sitting in the error
state.

**Still unverified:** behaviour with two deliberately seeded anchors held for a
long period (briefly observed and deterministic, but not soaked).

## The stratum ratchet — know this before deploying

Found by testing, not by reading the code. **The clock is RAM-only, so when the
stratum-0 node reboots the mesh loses its only real time source** — and what
happens next is a ratchet, because a rebooted node adopts from whichever
neighbour answers, and that neighbour is itself one hop out.

Since receivers never re-broadcast, **landing above stratum 1 is definitionally
"no stratum-0 node answered"**. Watch it climb across four reflashes:

```
11201: stratum=2 src=46a359bf     113201: stratum=3 src=29b8ad97
113301: stratum=3 src=e1256131    113401: stratum=2 src=46a359bf
```

One more round would hit `MN_TIME_MAX_STRATUM` (4) and the mesh would stop
distributing time altogether. The behaviour is *honest* — each hop really does
add latency, so the time really is getting worse — but it degrades silently,
which is the actual defect.

Two properties make this livable, both verified:

- **While an anchor lives, the mesh self-heals.** A stratum-0 announce always
  beats an equal-or-worse source, so the 15 minute refresh drags everyone back
  to stratum 1 without intervention.
- **Re-seeding is instant.** One `TIME SET` on any node took the bench from
  stratum 2/3/3/2 to 0/1/1/1 in **under 0.1 s** — all three followers adopted
  in the same breath.

So the fix is operational, and the firmware's job is to *say so*. Any node
adopting at stratum ≥ 2 now emits:

```
!WARN time-no-anchor stratum=2 — no stratum-0 node on this mesh; re-seed with TIME SET (tools/hcp.py synctime)
```

Verified by rebooting the anchor: the warning appeared 1.9 s later, on the
anchor itself, as it came back and adopted from a follower.

**Operational rule: re-seed after the anchor reboots.** This is now automatic
from the phone: `magnet_app` sends `TIME SET` on every connect
(`lib/magnet/mesh_session.dart`, `_seedClock`), and it reconnects far more
often than nodes reboot, so a mesh with the app in use stays permanently
anchored and the ratchet is unreachable in practice. From a laptop,
`tools/hcp.py synctime` does the same thing.

**Deliberately not done:** persisting the clock to NVS. A node that has been
powered off for an unknown interval would come back and announce a confidently
wrong time *at stratum 0*, which is worse than having no clock. If durability
across reboots is wanted, persist the anchor **role** and have the node refuse
to announce until re-seeded — that is a design call worth making explicitly
rather than defaulting into.

## Files

| File | What changed |
| --- | --- |
| `magnet_core.c` | clock state, stratum rules, announce/request, jitter timer |
| `include/magnet.h` | `mn_time_set/str/info/push/request`, `mn_time_is_set` |
| `magnet_link.c` | `TIME` verb: `SET`, `SYNC`, `PUSH`, bare query |
| `magnet_forth.c` | `mn-time!`, `mn-now`, `mn-time-push`, `mn-time-sync` |
| `tools/hcp.py` | `synctime` — computes host epoch + tz offset and seeds |
