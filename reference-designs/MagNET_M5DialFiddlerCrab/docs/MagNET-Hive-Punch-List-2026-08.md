# MagNET hive — priority punch list

**Date**: 2026-08-14
**Scope**: the ordered work list for the hive, reconciling two sibling projects into the
canonical design. **Supersedes §7 (S1–S7) of
[`MagNET-Hive-Design-Review-2026-08.md`](MagNET-Hive-Design-Review-2026-08.md)** — the
review's analysis (§1–§6) still stands; this document replaces its plan with one informed
by a full survey of both siblings (2026-08-14).

The three projects being reconciled:

| Project | What it is | Validation status |
|---|---|---|
| `MagNET_M5DialFiddlerCrab/` | The canonical hive: ruler, KV, roles, `craw_role_bundle` | Phase 4B multi-node validated 2026-04-25 |
| `MagNET_OTA_WaveC6LED/` | Forth-bundle OTA over HTTP: Ed25519, rollback, reporting | **D0–D4 closed on hardware**, incl. the failure path (commit `d436975`) |
| `MagNET_Thread_COaP_hanasu_esp32c6/` | Thread/CoAP mesh messaging, mesh time, HCP mgmt plane | fw 0.6.0-eg, 4-node soak 1,017/1,017 delivery, CI in Jenkins |

---

## 1. What each project contributes — and lacks

The reconciliation in one table. Each row is a capability the finished hive needs; ✅
means running validated code exists there today.

| Capability | Dial hive | WaveC6LED | Hanasu |
|---|---|---|---|
| Signed code delivery to live nodes | ✅ HMAC (weak, §4 of review) | ✅ **Ed25519** (TweetNaCl) | ❌ `SCRIPT` is unsigned |
| Failed-apply rollback | ❌ pollutes dictionary | ✅ savepoint + per-line eval | ❌ no upgrade path at all |
| Apply-result reporting | ❌ fire-and-forget | ✅ POST + convergence + poison protection | n/a |
| Survives reboot | ✅ NVS envelope + `apply_saved()` | ❌ **bundle is heap-only** | ✅ `SCRIPT` in NVS |
| Mesh transport (no AP, no server) | ❌ WiFi + ruler hub | ❌ HTTP to a server | ✅ Thread/CoAP multicast |
| Time without NTP | ❌ SNTP required | ❌ | ✅ stratum mesh time |
| Role/behavior model | ✅ roles + bundles (3 authored) | ❌ | partial: hooks + bots, no roles |
| Management plane | ruler + `push_bundles.py` | server check-in | ✅ HCP grammar, `hcp.py`, host SDK |
| Soak/CI harness | ❌ | ❌ | ✅ Jenkins + `soak.py` with gates |

Read down the columns: **each project is missing exactly what another one has already
validated on hardware.** Almost every P0/P1 item below is moving code, not writing it.

Two survey findings that reshape the old plan:

- **The Forth fork is smaller than feared.** Hanasu's `forth_core.{h,cpp}` is
  md5-identical to the Dial's canonical `ESPIDFORTH/components/forth/`. Only WaveC6LED
  diverged, and surgically: +67 lines in the `.cpp`, +26 in the `.h` — the savepoint
  struct/functions, `forth_error_count()`, and a NULL-safe `put_char`. Convergence (H1)
  is a two-way merge where one side is a strict superset. Trap: `forth_version.h` reads
  `0.2.0` in **both** the baseline and the fork, so the version string cannot tell you
  which core a project has.
- **The family now has two signature algorithms.** WaveC6LED vendored TweetNaCl because
  mbedTLS ships no EdDSA (`ota_verify.c:5-15`); Hanasu independently chose **ECDSA
  P-256** via mbedTLS for its message identity (`device_id = SHA256(pub)[0:4]`). "v2 =
  Ed25519" is therefore a decision to make deliberately (H3), not a default to inherit.

---

## 2. The punch list

Each item: what, where, effort class (**move** = relocate validated code, **new** = write
code, **decide** = a choice to record, **fix** = small correction), and how to validate.

### P0 — Foundations (order matters; H1 gates everything)

#### H1 — Converge the Forth core ✅ DONE 2026-08-14

**Completed in three moves.** (a) Wave's savepoint core merged into the canonical tree
as **ESPIDFORTH 0.3.0**; (b) ESPIDFORTH extracted to a standalone repo,
[IoTone/ESPIDFORTH](https://github.com/IoTone/ESPIDFORTH), full 14-commit history
preserved, vendored ESP32forth moved to `third_party/` with Apache-2.0 provenance, CI
matrix over all six envs; (c) the monorepo's copy replaced by a **git submodule**, and
all 18 consumers now reach one core — 6 pre-existing symlinks (incl. Hanasu), plus Wave
and the 11 stale-copy projects converted to symlinks. Builds validated on every chip
family: ESPIDFORTH C3, Hive Camera (ESP32), Hanasu C6, Dial ruler S3, E4TH Stamp C3,
Boombox S3, Wave C6 (raw IDF). Discovery en route: the tree had **19 forth instances in
3 variants**, not 3 copies — the 11-project variant was a strict ancestor (pre-
`forth_set_io`), nothing lost in convergence.

Original scope, for the record:

**Effort: move.** Promote WaveC6LED's `components/forth/` (baseline + savepoint) to the
shared component tree, symlinked like the `craw_*` components, consumed by Dial, Hanasu,
and Wave. Since Hanasu and the Dial run the identical baseline, this is one upgrade
applied three times, not a three-way merge.

- Source of truth: `MagNET_OTA_WaveC6LED/components/forth/forth_core.{cpp,h}` — the fork
  adds `forth_savepoint_t {dict_count, code_ptr, heap_used}`, `forth_save()`,
  `forth_restore()` (`forth_core.cpp:1313-1330`), `forth_error_count()`, and the
  `put_char` NULL-guard (a straight bug fix: `w_dot`/`w_cr`/`w_emit` crash any embedder
  that never called `forth_set_io`).
- Bump `forth_version.h` to **0.3.0** so the capability is detectable at runtime.
- Known risk from the review still applies: the Dial registers more FFI words than Wave —
  verify the registration shim before switching.
- Two documented caveats to carry into the shared header's comments: `forth_restore` is
  truncate-only and non-nestable, and it restores **dictionary state only** — not stacks,
  not `base`, not C-side state mutated through FFI words.

*Validate*: all three projects build; Dial REPL smoke test; Wave's D4 failure-path test
(`GOOD-WORD` gone after a failed apply) re-run unchanged.

#### H2 — Rollback in `craw_role_bundle` ✅ DONE 2026-08-14

**Completed.** The single `forth_eval` at the install step is now savepoint →
line-at-a-time eval (checking both the return code and the `forth_error_count()` delta)
→ `forth_restore` on first error, with the failing line captured in a new
`result->err_detail[80]` field (feeds H5/H4's `at` reporting). Component bumped to
0.2.0; spec's Install-pipeline table updated, including the documented limitation that
rollback undoes definitions, not FFI side effects. Validated two ways: device compile
(Capsule Scribe S3, savepoint symbols confirmed in the object), and a **9/9 host-side
functional test** compiling the real `forth_core.cpp` against ESP-IDF shims — failing
bundle returns `BUNDLE_ERR_EVAL` naming the bad line, a word defined before the failure
is gone afterward, a shadowed word reappears with its original body, and dict/code/heap
cursors restore exactly. Hardware re-run on the bench remains the final gate.

Original scope:

**Effort: move (~30 lines).** Replace the single `forth_eval()` call at
`craw_role_bundle.c:331` with the pattern from `magnet_ota/ota_apply.c:65`:
`forth_save` → line-at-a-time eval (`strtok_r` on `"\r\n"`) → `forth_restore` on first
error → error message carries the actual failing source line. Closes the review's §3
defect (R11).

Critical detail from the survey: **`forth_eval()` returns 0 even when the text references
undefined words.** Wave's loop detects failure by comparing `forth_error_count()` before
and after each line — port that mechanism, not just the savepoint calls.

*Validate*: install a bundle that defines one good word then references a missing one;
confirm `? GOOD-WORD` afterward and `BUNDLE_ERR_EVAL` naming the bad line.

#### H3 — DECIDE: bundle signature algorithm, then implement as RoleBundle v2 ✅ DONE 2026-08-14

**Completed — Ed25519 ratified and implemented.** New shared `magnet_crypto` component
(vendored TweetNaCl + hardware-RNG `randombytes`) consumed by both `craw_role_bundle`
and Wave's `magnet_ota` (whose private tweetnacl copy is deleted). Trust store is now
per-(author, alg) — `iotone-dev` carries both an HMAC and an Ed25519 entry for
migration. v2 bundles sign the same canonical pipe-string, just with Ed25519 (spec
updated, incl. the deliberate difference from Wave's hex-digest convention). Dev
keypair generated: pubkey in `keys.h`, seed committed as `scripts/dev_ed25519.key`
(dev posture, same as the HMAC secret). `sign_bundle.py --alg ed25519` works with the
`cryptography` package or a bundled pure-Python RFC 8032 fallback — both proven to
emit identical signatures. Validated: host round-trip (Python-signed bundle → C
`magnet_ed25519_verify` with the exact firmware pubkey → VALID; tampered
message/signature/wrong-key all INVALID), Scribe S3 firmware builds with
`magnet_ed25519_verify` + dev pubkey confirmed in the image, Wave C6 builds against
the shared component. HW-install of a v2 bundle on the bench = final gate.

Original decision framing:

**Effort: decide + move.** The choice, made explicit:

| Option | For | Against |
|---|---|---|
| **Ed25519 (TweetNaCl)** ← recommended | Already verifying + rejecting tampered payloads on C6 hardware; `keys.h` tags and `sig_alg` dispatch were built for it; small vendored dep | Second crypto lib alongside mbedTLS |
| ECDSA P-256 (mbedTLS) | Already Hanasu's identity; no vendored code | Not validated for bundle signing anywhere; DER/rs encoding decisions still to make |

Recommendation: **Ed25519 for bundle signing** (it is running code), while ECDSA P-256
remains Hanasu's *transport identity* — different layers, no conflict. Keep the HMAC path
alive during migration via the existing `sig_alg` dispatch.

Work: factor TweetNaCl + the sig-check into a shared `magnet_crypto` component (both
`magnet_ota` and `craw_role_bundle` consume it); add `TRUST_ALG_ED25519` entries to
`keys.h`; extend `scripts/sign_bundle.py` to sign with a private key. One quirk to
preserve from `ota_verify.c:144-150`: the server signs the **64-char lowercase hex SHA256
string as UTF-8 bytes**, not the raw digest — decide whether RoleBundle v2 keeps that
convention or signs the canonical pipe-string directly, and write it into the spec.

*Validate*: Wave's tamper-rejection test replayed against a v2 bundle on the Dial.

### P1 — One apply engine

#### H4 — Factor the shared verified-apply engine ✅ DONE 2026-08-14

**Completed.** The engine primitive now lives where it belongs — in ESPIDFORTH itself:
**`forth_eval_rollback(text, len, fail_line, fail_cap)`** (0.4.0) owns the savepoint →
per-line eval → rollback dance, works on a private copy (caller's text stays pristine
for persistence), and is length-aware — fully retiring the spec's phantom
`forth_eval_n`. Both consumers collapsed onto it. Each side then gained the other's
validated half:

- **Wave got persistence**: verified bundle text → NVS blob (persist-before-cfg-markers
  so a power cut re-applies rather than silently reverts), and `ota_apply_saved()` at
  boot re-evaluates it. The reboot gap — "looks upgraded, isn't" — is closed. Also
  fixed the vestigial `forth_heap_used() ? 0 : 0` log (was an H9 row).
- **The hive got reporting**: after every ROLE_GRANT install, nodes publish
  `applied:<node_id>` → `{"name","version","ok",…}` / `{"name","ok":false,"err",
  "field","at","ts"}` into the hive KV via `craw_role_bundle_format_applied_json()` +
  `craw_hive_node_kv_put()` — the ruler can finally tell a node running a role from
  one that failed to install it (visible via existing `kv-list`). Key is
  `applied:<id>` not the review's `role:<id>:applied` — the KV key cap is 32 chars.

Validated: host 9/9 now exercises the real `forth_eval_rollback`; builds green across
Scribe, Scribe_XR, Scribe_Redis, Boombox (S3) and Wave (C6, raw IDF). Discovery en
route (now H10): Scribe_XR, Scribe_Redis, and Boombox held stale private **copies** of
`craw_role_bundle` — converged to symlinks (verified byte-identical to pre-H2 root
first). HW gates: reboot-persistence on a Wave board; one grant→report round trip on
the bench.

Original scope:

**Effort: move + new (the glue).** With H1–H3 done, `magnet_ota` and `craw_role_bundle`
differ only in envelope parsing and where bytes come from. Factor one engine:
**verify → apply-with-rollback → persist → report**, where each side contributes the half
it already validated:

- From the hive side: **persistence** — NVS envelope + `craw_role_bundle_apply_saved()`
  at boot. This fixes WaveC6LED's worst unwritten defect: applied bundles live only in a
  heap `malloc` (`ota_verify.c:40`), so a power cycle silently reverts the dictionary
  while NVS `CFG_APPLIED_ID` still reports the device as up to date. (Its 960 KB SPIFFS
  `storage` partition was reserved for exactly this and never used.)
- From the Wave side: **reporting and convergence** — apply-result with ok/err + failing
  line; persist-before-report ordering so a lost report converges to noop rather than
  re-applying; poison protection (3 identical failures on one release → stop refetching
  until a different one is offered). This fixes the hive's §6 silence: the ruler finally
  learns whether a grant took. On the hive transport the report is the KV convention from
  the review: `KV_PUT role:<node_id>:applied {"name","version","ok","err","at","ts"}`.

*Validate*: Wave's full D4 loop and the Dial's `ROLE_GRANT` install both green through
the shared engine; power-cycle a Wave board and confirm the bundle auto-resumes.

#### H5 — Role lifecycle vocabulary

**Effort: new (spec + small host code).** The `role-init` / `role-tick` / `role-stop` /
`role-status` + `tick_ms` convention from the review §5 — host firmware owns the timer,
bundles never block. This is what unblocks the 8 unwritten roles (seven are inherently
loops the current bundle model cannot express).

Hanasu already field-tested the hazards this convention will meet — adopt its patterns
rather than rediscovering them:

- **Non-reentrancy funnel**: every eval goes through one choke point with a fixed lock
  order (Hanasu's `mn_forth_exec()`, TX mutex → engine mutex). The tick timer must use
  the same funnel as REPL/install.
- **Circuit breaker**: Hanasu caps hook firings at 5/s and measurably stopped a two-node
  echo volley (0.6 msg/s instead of ~50/s). Cap `role-tick` overruns the same way — a
  tick that's still running when the next fires is skipped and counted, not queued.
- **Bind by word name, not xt** — survives redefinition and rollback.

Retrofit the three authored bundles (`spawn`, `scribe-extra`, `spy-snapper` — the latter
stays valid by simply omitting `role-tick`).

*Validate*: a deliberately slow `role-tick` bundle neither wedges the REPL nor stacks
timer callbacks; `role-status` reachable from both REPL and ruler query.

### P2 — Transports & time

#### H6 — Transport plumbing: one engine, three transports

**Effort: new (two vtable impls).** WaveC6LED's `magnet_transport_t` vtable
(`include/magnet_transport.h:28-44`, sole impl `transport_ip.c`) was designed for exactly
this. Add:

1. **Hive-KV transport** — pull `bundle:<name>` from the Scribe (what `craw_role_bundle`
   does today, refactored behind the vtable).
2. **CoAP/Thread transport** — bundle delivery over Hanasu's mesh. This **answers
   Hanasu's own Open Question #4** ("OTA over M2M binary transfer"), which its design
   doc left unresolved. Forth-source bundles need no dual-slot partition table — Hanasu's
   single-slot layout is fine as-is. The 4-bit fragment field caps transfers at ~17 KB,
   comfortably above the 4 KB bundle cap.

Then retire Hanasu's unsigned `SCRIPT SET` as the upgrade path: `SCRIPT` remains a dev
convenience, but fleet-delivered code arrives only as signed RoleBundle v2 envelopes
through the shared engine. This also resolves Hanasu §12.9 open decision #2 (NVS vs
LittleFS for scripts): bundles persist wherever the shared engine persists them.

Also carried from Wave's own TODO list, relevant once transports multiply: HTTP transport
is still plaintext with a bearer token in the clear (R1 = HTTPS).

*Validate*: same signed Eye bundle delivered three ways (HTTP, KV, CoAP) installs
identically; tampered bundle rejected on all three.

#### H7 — Mesh time ⇄ hive time

**Effort: move + fix.** The hive's HMAC accepts ±30 s skew and currently leans on SNTP;
Hanasu's stratum mesh time (`docs/MESH-TIME.md`) delivers ~±1 s with **no NTP at all** —
exactly what Thread-only or AP-less hive segments need. Two work items:

1. Adopt stratum time as an alternative hive time source alongside SNTP (the existing
   post-WiFi bringup order — BLE teardown → time sync → hive start — is unchanged; only
   the sync source becomes pluggable).
2. **Fix the stratum ratchet first**: today an anchor reboot makes the mesh silently
   climb stratum 2→3→4 and stop distributing time. MESH-TIME.md's own suggested fix is
   right: persist the anchor *role* (not the clock — a stale clock re-announced at
   stratum 0 is worse than none), and refuse to announce until re-seeded. A hive trusting
   this for HMAC windows can't accept silent degradation.

*Validate*: soak-length run with a mid-run anchor reboot; HMAC joins keep succeeding.

### P3 — Authoring & hygiene

#### H8 — Author the 8 role bundles

**Effort: new — and now genuinely just authoring**, once H5 lands. Order: **Eye first**
(Role 11) — it feeds the Milk-V Duo / M5Stamp C3U sidecar work, its `role-tick` shape is
the worked example in the review §5, and the sidecar's gating facts are already in hand
(`ttyS1` free, 1.5 Mbaud ceiling). Then Worker, Parrot, Beeper, Pet, and the rest.

*Validate*: each bundle installs via `ROLE_GRANT`, reports applied, survives reboot,
and `role-status` answers.

#### H9 — Small fixes (batchable, any time)

| Fix | Where |
|---|---|
| READY banner says `fw=0.5.0-ee`, CAPS says `0.6.0-eg` — any inventory keyed on the banner reads the wrong version | Hanasu `src/main.c:102` vs `magnet_link.c:98` |
| ~~Vestigial `forth_heap_used() ? 0 : 0` — success log always prints dict `0`~~ ✅ fixed in H4 | Wave `ota_apply.c` |
| Spec names `forth_eval_n()`, which exists nowhere; code calls `forth_eval` — implement the length-aware variant (fix the code, not the spec) | `MagNET-RoleBundle-v1.md` §Install pipeline |
| "Known caveats" section frozen at E-B (claims plaintext-only, etc.), contradicted by E-D/E-G above it in the same file | Hanasu `firmware-idf/README.md` |
| `report()` sanitizes by char-substitution rather than JSON escaping, truncates at 192 B — fine for Wave's server, verify against the hive KV consumer | Wave `ota_apply.c:46-53` |

---

#### H10 — Converge the drifted `craw_*` component copies (added 2026-08-14)

**Effort: audit + merge.** H4 tripped over the forth-core disease one layer up: node
projects hold private copies of shared `craw_*` components. Audit (2026-08-14):
**15 genuinely-drifted copies** across 5 projects (Scribe_XR/Redis: `craw_ble_provision`,
`craw_imu`, `craw_redis`, `craw_wifi`; Voice_XR: `craw_imu_mpu6886`, `craw_mic`;
Boombox: `craw_audio`, `craw_wifi`; Vitals_E4TH: `craw_bh1750`, `craw_mr60bha2`,
`craw_status_led`) plus **9 identical copies** safe to symlink immediately
(`craw_hive` ×3, `craw_mqtt` ×2, `craw_nvs` ×3, `craw_ble_provision` ×1). The drifted
ones need per-component diff review — some divergence is real feature work (e.g.
Voice_XR's M5GFX lgfx::i2c reuse) that should merge *into* the root component, H1-style.
The `craw_role_bundle` ×3 case is already done (H4).

## 3. Deferred — deliberately

Unchanged from the review, plus one addition:

- **R6 real consensus** — the auto-accept stub is honest; not demo-critical.
- **R16–R18 NDN / distributed custody / P2P chat** — a phase, not a step. "Bundles live
  on the Scribe" remains the right first move toward it.
- **Full ESP32forth v7.0.8.0 port** — Hanasu's E-G decision concurs: the stub stays. The
  C-component-plus-thin-vocabulary boundary both siblings honor exists so that an engine
  swap moves only the registration shims.
- **Hanasu 32-node soak** — planned in its README §E-G; its own track, not hive-blocking.
- **Savepoint scope extension** (stacks, C-side FFI state) — only if a real bundle bites
  it; the dictionary-only caveat is documented in H1.

---

## 4. First move

~~H1~~ ~~H2~~ ~~H3~~ ~~H4~~ all **done 2026-08-14**. Next up: **H5**, the role
lifecycle vocabulary (`role-init/tick/stop/status` + `tick_ms`) — the item that
unblocks the 8 unwritten roles. H10 (craw_* drift convergence) can proceed in
parallel any time.
