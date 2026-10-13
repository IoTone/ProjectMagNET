# MagNET hive — design review and next steps

**Date**: 2026-08-07
**Scope**: where the hive spec actually stands after Phase 4, the divergence introduced by
`MagNET_OTA_WaveC6LED`, and an ordered plan for the Forth code-upgrade path.
**Status**: review + proposal. Nothing here is implemented yet.

---

## 1. The headline

Two independent code-upgrade systems now exist in this repo, built months apart, and
**neither knows the other exists**:

| | Hive path (`craw_role_bundle`) | OTA path (`magnet_ota`) |
|---|---|---|
| Project | `MagNET_M5DialFiddlerCrab/` | `MagNET_OTA_WaveC6LED/` |
| Spec | `MagNET-RoleBundle-v1.md` | RobotARme repo, `wavec6led-ota-plan.md` |
| Transport | `KV_GET bundle:<name>` from the Scribe | HTTP pull from a RobotARme server |
| Signature | HMAC-SHA256, shared hive secret | **Ed25519**, tweetnacl vendored |
| Failed apply | **Leaves partial definitions behind** | **Rolls back cleanly, names the bad line** |
| Result reporting | none | reports APPLIED vs FAILED to the server |
| Convergence | n/a | check-in loop stops asking once up to date |

Read that table in one direction and the conclusion is uncomfortable: **the newer project
solved the two hardest correctness problems, and the canonical hive spec still has both
bugs.** The work to fix the hive path is mostly *moving code that already runs on
hardware*, not inventing anything.

This document argues for collapsing them into **RoleBundle v2: one envelope, one apply
engine, two transports.**

---

## 2. What Phase 4 actually delivered

Genuinely done and hardware-validated:

- **Milestone A** — BLE provisioning across four chip families (C3, classic ESP32, S3).
- **Milestone B** — mDNS discovery + HMAC-SHA256 join, multi-node validated 2026-04-25
  with three chip families and three roles against one Dial ruler.
- **Milestone C steps 1–4** — KV protocol layer, `craw_role_bundle` component,
  `ROLE_GRANT` install pipeline, and two bundle-delivery paths (compiled-in bootstrap,
  and `push_bundles.py` over mDNS).

That is a real distributed system. The remainder of this document is about the gap
between it and the design section's stated requirements.

### Requirements still unmet

| Req | Claim | Reality |
|---|---|---|
| R6 | Join accepted "based on consensus of the hive" | Ruler auto-accepts any valid HELLO. Consensus is a stub. |
| R10 | Node validates "the author/signer" | v1 signs with the shared hive secret — every node can forge any author. See §4. |
| R11 | Version check, rollback support | Monotonic check exists. **Rollback does not.** See §3. |
| R13 | Role may reconfigure physical interfaces | No mechanism; bundles can only call whatever FFI words the host already registered. |
| R16/R17 | Shared memory via NDN/CCN, distributed custody | KV table: 16 entries × 3 KB, **in the ruler's RAM**. Hub-and-spoke, the opposite of NDN. |
| R18 | P2P `/chat/PEERID` between nodes | Untouched. All traffic is node↔ruler. |

Roles: 4 of 12 exist as running code (Ruler, Scribe, Spy, Boombox). Three bundles are
authored (`spawn`, `scribe-extra`, `spy-snapper`). **Eight role bundles remain unwritten**
— and §5 argues they are blocked on a missing convention, not on authoring effort.

---

## 3. Defect: a failed bundle install leaves the node polluted

`craw_role_bundle.c:331` is the entire install step:

```c
/* Install: forth_eval against the decoded source. */
int frc = forth_eval((const char *)src);
if (frc != 0) {
    ESP_LOGW(TAG, "forth_eval failed rc=%d for bundle '%s'", frc, name);
    /* ... returns BUNDLE_ERR_EVAL */
}
```

One call, whole blob. The Forth dictionary is append-only, so a bundle that defines ten
words and fails on the eleventh line leaves **all ten definitions permanently resident**.
The install reports failure; the node is left in a state that is neither the old role nor
the new one. Every subsequent failed install layers more debris.

`magnet_ota/ota_apply.c` already solves this exactly:

```
[   ota_apply: applying release 2507 (201 bytes) at dict=69
[   ota_apply: apply FAILED, rolled back to dict=69 code=0
[            — failed at: : BAD-WORD THIS-WORD-DOES-NOT-EXIST ;
```

The mechanism is a savepoint over the two dictionary cursors, evaluated **line by line** so
the error message names the actual offending source:

```c
typedef struct { int dict_count; int code_ptr; ... } forth_savepoint_t;
void forth_save(forth_savepoint_t *sp);
void forth_restore(const forth_savepoint_t *sp);
```

WaveC6LED's README documents verifying this on hardware: `GOOD-WORD`, defined on the line
*before* the failure, is gone afterward — `? GOOD-WORD`. A partial apply leaves no trace.

### The blocker: the Forth cores have forked

`forth_save` / `forth_restore` exist only in `MagNET_OTA_WaveC6LED/components/forth/`.
The Dial's copy at `ESPIDFORTH/components/forth/forth_core.h` exposes only:

```
forth_init  forth_repl  forth_set_io  forth_eval  forth_heap_used
forth_heap_free  forth_register_word  forth_push  forth_pop  forth_deinit
```

No savepoint. So the Dial **cannot** get rollback until the two copies converge. That
convergence is the first item in the plan for exactly this reason.

> **Spec drift, minor:** `MagNET-RoleBundle-v1.md` §Install pipeline documents the install
> step as `forth_eval_n(decoded_src, decoded_len)`. No such function exists in either
> `forth_core.h`; the code calls the NUL-terminated `forth_eval`. The length-aware variant
> is what the spec *should* want — fix the code, not the spec.

---

## 4. Defect: the v1 trust model cannot do what R10 asks

R10 requires a node to "check the author/signer." RoleBundle v1 signs bundles with
`CRAW_HIVE_DEV_SECRET` — the same 32-byte key every node holds for transport HMAC.

The spec is honest about this ("anyone who can verify can also sign"), but the consequence
is worth stating plainly: **with a symmetric key, author verification is theater.** Any
node in the hive — or anyone who dumps one node's firmware — can mint a bundle claiming
any author. A compromised Spy can publish a malicious `scribe` bundle that every node
accepts. The signature proves hive membership, nothing more.

Ed25519 was documented as "v2 (planned)". It is no longer planned — it is **running code**:
`magnet_ota` vendors tweetnacl, and WaveC6LED's README documents it provably rejecting a
tampered release on hardware.

The migration surface is already built. `keys.h` is a tagged trust store:

```c
{ "iotone-dev", TRUST_ALG_HMAC_SHA256, CRAW_ROLE_BUNDLE_DEV_HMAC_KEY, 32 },
```

and the envelope carries `sig_alg` precisely so both schemes can coexist during migration.
This is wiring, not research.

---

## 5. The real blocker on the remaining 8 roles

Every unwritten role is inherently a loop or an event responder. Worker polls for tasks.
Parrot echoes. Beeper waits for a trigger. Pet barks at strangers. Eye watches.

But bundles cannot loop. From the spec:

> Bundles SHOULD avoid blocking forever at top level — the install task is shared and a
> runaway bundle blocks subsequent installs.

And from `bundles/spy-snapper.forth`, in the author's own words:

```forth
\ Real periodic-capture loops should use cooperative Forth tasks once those land;
\ for v1 we keep it as a one-shot so the install doesn't tie up the Forth REPL.
```

There are no task words. The full ESP32forth engine has a `YIELD` hook
(`ESP32forth.ino:1214`, `g_sys->YIELD_XT`), but that engine is still Arduino-bound and
unported; the stub engine everything actually runs has no scheduler at all.

So "author 8 more bundles" is not really an authoring task. **Seven of the eight roles
cannot be expressed in the current bundle model.** That is the actual state of Milestone C.

### Proposal: a role lifecycle vocabulary, not a scheduler

Building cooperative multitasking into the Forth engine is a large, risky change. It is
also unnecessary. Adopt a convention instead — every role bundle defines up to four words,
and the **host firmware owns the timer**:

| Word | Called when | Contract |
|---|---|---|
| `role-init` | once, at install | Set up state. May fail; failure rolls the bundle back. |
| `role-tick` | on a host FreeRTOS timer | **Must return promptly.** One unit of work. |
| `role-stop` | on role change or shutdown | Release anything `role-init` claimed. |
| `role-status` | on demand (REPL, ruler query) | Print or push one status line. |

The host defines the cadence (a `tick_ms` field in the envelope, defaulting to something
conservative like 1000 ms). A bundle that only wants one-shot behaviour simply omits
`role-tick` — which keeps `spy-snapper` valid as written.

This gets the remaining roles writable without touching the engine, and it keeps the
"bundle must not block" rule enforceable rather than aspirational. If real Forth tasks
land later, `role-tick` remains a sensible contract on top of them.

### Worked example — Role 11 Eye, as a bundle

The Eye is the immediate consumer, and it exercises the convention properly:

```forth
\ Role: eye v1.0.0 — poll the attached retina, publish sightings to the hive.
\ caps_req: ["camera","detect"]

variable eye-seq

: role-init    eye-open  0 eye-seq ! ;
: role-stop    eye-close ;

: role-tick
   eye-detect              ( -- n-objects )
   ?dup if
     eye-publish           \ KV_PUT eye:<mac4>:last
     1 eye-seq +!
   then ;

: role-status  ." eye seq=" eye-seq @ . cr ;
```

Note what this does *not* contain: no sleep, no loop, no blocking. The host calls
`role-tick` every `tick_ms` and the bundle stays a pure description of behaviour. That is
the property that makes a bundle safe to hot-swap.

---

## 6. Missing: the hive never learns whether a grant took

The OTA path reports back. Its log line `apply-result reported: release 2362 OK` is the
difference between a deployment system and a fire-and-forget broadcast — and WaveC6LED's
README calls out the failure mode it prevents: *"a failed update cannot make a device look
upgraded."*

The hive path has no equivalent. The ruler sends `ROLE_GRANT` and learns nothing. It cannot
distinguish a node running the new role from one that failed to install it, and the peer
table shows the *requested* role rather than the *running* one.

This needs no new message type. A KV convention suffices:

```
KV_PUT  role:<node_id>:applied   {"name":"eye","version":"1.0.0","ok":true,
                                  "ts":1754500000}
KV_PUT  role:<node_id>:applied   {"name":"eye","version":"1.0.0","ok":false,
                                  "err":"BUNDLE_ERR_EVAL",
                                  "at":": BAD-WORD MISSING-THING ;"}
```

The `at` field is only populatable *after* §3's per-line rollback lands — another reason
that item comes first.

---

## 7. Ordered plan

> **Superseded 2026-08-14** by [`MagNET-Hive-Punch-List-2026-08.md`](MagNET-Hive-Punch-List-2026-08.md),
> which re-prioritizes S1–S7 as H1–H9 after a full survey of both sibling projects
> (notably: only WaveC6LED forked the Forth core — Hanasu's is identical to the Dial's —
> and the family now has two signature algorithms to reconcile). The analysis in §1–§6
> above still stands.

Each step is independently shippable and leaves the tree working.

**S1 — Converge the Forth core.** Promote WaveC6LED's `forth_core` (the one with
savepoints) to a shared `components/forth/`, symlinked like the `craw_*` components.
Today the Dial's engine lives at the project root under `ESPIDFORTH/`, outside the shared
component tree, which is why it drifted. *Prerequisite for everything below.*
Risk: the Dial has more FFI words registered than WaveC6LED; verify the registration shim
before switching.

**S2 — Rollback in `craw_role_bundle`.** Replace the single `forth_eval` with
save → per-line eval → restore-on-first-error, lifted from `ota_apply.c`. Populate the
failing source line into the error path. Roughly 30 lines. **Fixes the R11 gap.**

**S3 — Ed25519 as RoleBundle v2.** Vendor tweetnacl behind a shared `magnet_crypto`
component (both consumers need it), add `TRUST_ALG_ED25519` entries to `keys.h`, keep the
HMAC path alive for migration via the existing `sig_alg` dispatch. Extend
`scripts/sign_bundle.py` to sign with a private key. **Fixes the R10 gap.**

**S4 — Role lifecycle vocabulary.** Specify `role-init` / `role-tick` / `role-stop` /
`role-status` plus a `tick_ms` envelope field. Add the host-side timer to `craw_role_bundle`.
Retrofit the three existing bundles. **Unblocks the remaining 8 roles.**

**S5 — Apply-result reporting.** The `role:<node>:applied` KV convention from §6, plus a
ruler-side display of running-vs-requested role.

**S6 — Author the 8 role bundles.** Now genuinely an authoring task. Eye (Role 11) is the
natural first, since the Duo/Stamp sidecar work needs it anyway.

**S7 — Unify the transports.** With S1–S3 done, `magnet_ota` and `craw_role_bundle` differ
only in where bytes come from. Factor the shared apply engine; let the hive path pull from
the Scribe and the RobotARme path pull over HTTP, through one verified-apply core.

### Deliberately deferred

- **R6 real consensus.** Interesting, and not on the critical path for a working demo. The
  auto-accept stub is honest about what it is.
- **R16–R18 Named Data Networking.** The current KV table is ruler-centric — 16 entries in
  one node's RAM. Genuine NDN means content-addressed names, per-node caches with freshness,
  and custody transfer. That is a phase, not a step. The "bundles live on the Scribe"
  decision is the right first move toward it.
- **Full ESP32forth v7.0.8.0 port.** The Arduino-dependency strip is a large job. The
  C-component-plus-thin-vocabulary architecture (used by both `magnet_ota` and
  `craw_role_bundle`) exists specifically so that when the engine is swapped, only the
  registration shim moves. Keep honouring that boundary.

---

## 8. How the Eye lands in this

The Milk-V Duo 256M Eye (see `device-profile-MilkV-Duo256M.md`) is shelved as a *hive
member* because it has no radio, and is being revived as an **M5Stamp C3U sidecar**: the
Stamp is the hive member and carries the radio, the Duo is its retina over UART.

That plan depends on nothing in this document — but it lands much better *after* S4. The
Eye's natural shape is "poll the retina on a cadence, publish what changed," which is
`role-tick` almost verbatim (§5). Without S4 the Eye has to be firmware rather than a
bundle, which forfeits the whole point of the biologic layer.

Bench findings from 2026-08-07 that bear on it: `ttyS1` is free and the UART ceiling is
**1.5 Mbaud**, not the 921600 originally assumed — a 200 KB full-res JPEG crosses in ~1.4 s,
so `/capture` compatibility with the existing Spy node is achievable rather than a
compromise.

---

## 9. Summary

The hive works. The two things it cannot currently do are **fail safely** and **prove who
signed a bundle** — and both were solved, on hardware, in a sibling project that the
canonical spec has not absorbed. S1–S3 close that gap by moving existing code. S4 is the
one genuinely new idea here, and it is the cheap version of a hard problem: a calling
convention instead of a scheduler.

Do those four and Milestone C is finishable as an authoring exercise, which is what it was
always described as.
