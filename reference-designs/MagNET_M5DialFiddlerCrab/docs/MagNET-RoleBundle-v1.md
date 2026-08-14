# MagNET Role Bundle Format v1

**Status**: draft (Phase-4 Milestone C step 2).
**Scope**: serialized format + signing + install pipeline for the Forth role payloads delivered to hive nodes via `ROLE_GRANT`. Implementation lives in [`../components/craw_role_bundle/`](../components/craw_role_bundle/).

A role bundle is a signed JSON envelope carrying ESPIDFORTH source code that the receiving node executes via `forth_eval_n()`. Bundles are how a hive teaches its nodes new behavior at runtime — without reflashing.

## Goals

- Self-contained: a bundle has everything a node needs to install + run a role (source, version, signature).
- Verifiable: signature catches tampering and lets a node refuse a bundle from an unauthorized publisher.
- Portable: any chip family that runs ESPIDFORTH and the matching FFI words can install any bundle whose `caps_req` it advertises.
- Compact: typical bundles are <2 KB, fitting comfortably in a single hive `KV_DATA` frame (3 KB cap).

## Wire format

JSON object. All fields required unless noted.

```json
{
  "name":      "spy",
  "version":   "1.0.3",
  "min_proto": 1,
  "author":    "iotone-dev",
  "caps_req":  ["camera", "jpeg"],
  "deps":      [],
  "crc32":     "a1b2c3d4",
  "sig_alg":   "hmac-sha256",
  "sig":       "f17b...",
  "src_b64":   "OiBzcHktbG9vcCAuLi4="
}
```

### Field semantics

| Field | Type | Purpose |
|---|---|---|
| `name` | string ≤ 32 chars | Role name. Conventionally `spy`, `scribe`, `beeper` — one of the design-section roles. May be a custom role for project-specific bundles. |
| `version` | string `MAJOR.MINOR.PATCH` | Semver. Node refuses any bundle whose version is older than the currently-installed one for the same role (monotonic upgrade by default). |
| `min_proto` | integer | Minimum hive-protocol version required. Currently `1`. Bundles built for a future protocol are refused. |
| `author` | string ≤ 32 chars | Identifies which trusted-key the signature must validate against. Lookup in node's compile-time trust store. |
| `caps_req` | array of strings | Capabilities the role needs. The node refuses install if its own caps don't cover this list. (E.g. spy bundle requires `camera` — refused on a Scribe.) |
| `deps` | array of strings | Other bundles the role expects to be installed first. Empty for v1; the install pipeline doesn't enforce ordering yet. |
| `crc32` | hex string | CRC-32 of the **decoded** Forth source. Independent of signature, defends against base64 corruption in transit. |
| `sig_alg` | string | Signature algorithm: `hmac-sha256` (v1) or `ed25519` (v2, implemented 2026-08-14). |
| `sig` | hex string | Signature over the canonical signing input (below). Length depends on alg. |
| `src_b64` | base64 string | Forth source code, base64-encoded. Decoded length ≤ 4096 bytes for v1 (single-frame KV value cap). |
| `tick_ms` | integer, optional | H5 lifecycle: cadence for the host-driven `role-tick` timer. Default 1000, clamped to 100..60000. Like `caps_req`, sits **outside** the v1 canonical signing input — a known limitation of the fixed six-field signature. |

### Canonical signing input

```
"<name>|<version>|<min_proto>|<author>|<crc32>|<src_b64>"
```

Pipe-delimited concatenation of those six fields, in that exact order, no whitespace. This is the byte string the publisher signs and the node verifies.

For `sig_alg = "hmac-sha256"`:
```
sig = HMAC-SHA256(shared_key, signing_input)  # 32 bytes hex-encoded
```

For `sig_alg = "ed25519"` (v2):
```
sig = Ed25519-Sign(author_privkey, signing_input)  # 64 bytes hex-encoded (128 hex chars)
```

Note the deliberate difference from the WaveC6LED OTA server convention (which signs
the hex SHA256 of the payload): role bundles sign the **canonical pipe-string
directly** — same input for both algorithms, only the primitive changes.

## Trust model

### v1 — HMAC with the hive secret

Bundles are signed with the same 32-byte `CRAW_HIVE_DEV_SECRET` used for hive-protocol HMACs. **This means anyone who can verify can also sign** — the model is "nodes trust holders of the shared secret to publish bundles." Adequate for development and demo deployments where all hardware ships with the same key. Inadequate for production: a leak of one node's firmware leaks the publisher key.

### v2 — Ed25519 with per-author public keys (implemented 2026-08-14, punch-list H3)

Each `author` has a 32-byte public key baked into the node's firmware via `components/craw_role_bundle/keys.h`. Authors hold private keys offline. Adding a new trusted author requires reflash. This matches the threat model where individual nodes can be physically compromised but firmware integrity is preserved (signed boot, etc.).

The `sig_alg` field exists from v1 specifically so a single node can support both schemes during the migration: receive v1 bundles signed with HMAC, and v2 bundles signed with Ed25519, choosing the verification path at runtime. Implementation notes:

- The trust store may hold one entry **per (author, algorithm)** pair; lookup matches
  both. The dev store carries `iotone-dev` twice — HMAC (legacy) and Ed25519.
- Verification goes through the shared **`magnet_crypto`** component (vendored
  TweetNaCl — mbedTLS ships no EdDSA), the same implementation the WaveC6LED OTA
  path uses.
- The dev keypair: public half in `keys.h` (`CRAW_ROLE_BUNDLE_DEV_ED25519_PUB`),
  private seed in `scripts/dev_ed25519.key` — committed on purpose as a DEV key,
  same posture as the dev HMAC secret. Production authors keep seeds offline.
- `scripts/sign_bundle.py --alg ed25519` signs with the dev seed by default, or
  `--key-file <hex-seed>` for a production key. It uses the `cryptography` package
  when importable and otherwise a bundled pure-Python RFC 8032 fallback; both
  produce identical (deterministic) signatures.

## Install pipeline

`craw_role_bundle_install_from_json(json_str)` runs the following steps. Any failure aborts and returns a specific error code:

| Step | Failure code | What it catches |
|---|---|---|
| Parse JSON envelope | `BUNDLE_ERR_PARSE` | Malformed JSON, missing required fields |
| Validate `min_proto` | `BUNDLE_ERR_PROTO` | Bundle requires a future hive-protocol version |
| Look up author in trust store | `BUNDLE_ERR_AUTHOR` | Unknown publisher |
| Verify signature | `BUNDLE_ERR_SIG` | Tampered envelope, wrong key, drifted canonical-string layout |
| Base64-decode `src_b64` | `BUNDLE_ERR_BASE64` | Corrupt encoding |
| CRC32 over decoded source matches `crc32` | `BUNDLE_ERR_CRC` | Source mutated after signing (shouldn't happen if sig passed; double-defends) |
| Check `caps_req` ⊂ node's `caps` | `BUNDLE_ERR_CAPS` | Spy bundle on a Scribe-only node |
| Compare `version` against persisted last-version for `name` | `BUNDLE_ERR_VERSION` | Downgrade attempted (allowed only with explicit `--allow-downgrade` flag, future) |
| Savepoint → line-at-a-time `forth_eval` → restore on first error | `BUNDLE_ERR_EVAL` | Undefined word or syntax error anywhere in the source. **The dictionary is rolled back to its pre-install state** — no partial definitions survive, and any word a partial install had shadowed is visible again. The failing source line is reported in `result->err_detail`. |
| Persist envelope to NVS | `BUNDLE_ERR_NVS` | NVS write fails (rare; bundle is still active in RAM) |

On success, the bundle's Forth words are registered in the global vocabulary and its top-level body has been executed once. The node persists `name`, `version`, and the full envelope so the same role auto-resumes on next boot without re-fetching.

The eval step's rollback (added 2026-08-14, punch-list H2) relies on `forth_save()` /
`forth_restore()` / `forth_error_count()` from ESPIDFORTH ≥ 0.3.0. The mechanism is a
truncation of the append-only dictionary's fill cursors — it undoes *definitions*, not
side effects: stack contents and any C-side state mutated through FFI words during the
partial install are not restored. Bundles that mutate state before their last definition
line should be written to tolerate that (or defer side effects to `role-init`, once the
role lifecycle convention lands).

## Apply-result reporting (punch-list H4, 2026-08-14)

After every install attempt triggered by `ROLE_GRANT` — success *or* failure — the node
publishes the outcome into the hive KV so the ruler can distinguish a node **running**
a role from one that failed to install it:

```
key   = applied:<node_id>            (KV key cap is 32 chars, so the earlier
                                      role:<id>:applied sketch didn't fit)
value = {"name":"eye","version":"1.0.0","ok":true,"ts":1755180000}
value = {"name":"eye","ok":false,"err":-9,"field":"src",
         "at":": BAD-WORD MISSING-THING ;","ts":1755180000}
```

`craw_role_bundle_format_applied_json()` builds the value (sanitizing the failing line
for JSON embedding); the node main sends it via `craw_hive_node_kv_put()`. Visible on
the ruler through the existing `kv-list` / `kv-get` words. The `at` field carries the
failing source line captured by the rollback path.

## NVS persistence

| Namespace | `craw_role_bundle` |
|---|---|
| Per-role keys | `n:<name>` → version string, `b:<name>` → full envelope JSON |
| Size limit | 4 KB per blob (NVS hard limit). Larger bundles are accepted at runtime but won't auto-resume on reboot. |

`craw_role_bundle_apply_saved()` is called early in boot — iterates the namespace, re-installs each persisted bundle. Useful pattern: install a bundle once via `ROLE_GRANT` (or REPL for testing), then reboot and watch it auto-resume.

## Authoring a bundle

Use [`scripts/sign_bundle.py`](../scripts/sign_bundle.py):

```bash
echo ': hello-spy ." Hello from the spy role" cr ;' > /tmp/spy.forth
python sign_bundle.py /tmp/spy.forth \
  --name spy \
  --version 1.0.0 \
  --author iotone-dev \
  --caps-req "camera,jpeg" \
  > spy.json

# v2 — Ed25519 (preferred; dev key picked up automatically):
python sign_bundle.py /tmp/spy.forth --alg ed25519 \
  --name spy --version 1.0.0 --author iotone-dev \
  --caps-req "camera,jpeg" > spy.json
```

The output `spy.json` is ready to either:
- POST to a Scribe via `KV_PUT key=bundle:spy`, or
- Embed in a Ruler firmware as a bootstrap fallback (Step 3+).

## Forth role conventions — the H5 lifecycle (2026-08-14)

Bundles cannot loop, and they don't need to: a bundle defines up to four
conventional words and the **host firmware owns the timer**.

| Word | Called when | Contract |
|---|---|---|
| `role-init` | once, at install (after the source evals) | Set up state. **Failure rolls the whole bundle back** — a role that cannot set up must not half-exist. |
| `role-tick` | every `tick_ms` from a host task | **Must return promptly.** One unit of work. The task is sequential, so an overrunning tick delays the next one (and increments a counter) — it never stacks. |
| `role-stop` | before this role is replaced, or on shutdown | Release anything `role-init` claimed. Called while the *old* definitions are still live. |
| `role-status` | on demand (REPL, ruler query) | Print or push one status line. |

All four are optional — a one-shot bundle simply omits `role-tick`. Words are invoked
by **name** through `forth_eval()`, so redefinition and rollback are always respected.
One active tick role per node (v1). Requires ESPIDFORTH ≥ 0.5.0 (`forth_word_exists`
+ internally-serialized engine) and craw_role_bundle ≥ 0.3.0.

```forth
\ Role: spy-snapper — periodic capture through the lifecycle.
: snap        cam-snap drop ;
: role-init   snap ;          \ one confirmation capture at install
: role-tick   snap ;          \ periodic; cadence = envelope tick_ms
: role-status ." spy-snapper ticking" cr ;
```

Note what this contains no trace of: no loop, no sleep, no blocking. That is the
property that makes a bundle safe to hot-swap.

Top-level execution at install time still works but is discouraged — put install-time
behavior in `role-init`, where a failure is rolled back and reported.

### Hive KV words for bundles (H8)

The per-node `kv-get` / `kv-put` REPL words are interactive (they prompt on the
console) and would hang a `role-tick`. Bundles use the stack-based trio instead,
registered by `craw_role_bundle_register_hive_words()` on every bundle-capable node:

| Word | Stack effect | Semantics |
|---|---|---|
| `hkv-put$` | `( v-addr v-len k-addr k-len -- )` | Fire-and-forget KV_PUT |
| `hkv-get$` | `( k-addr k-len -- v-addr v-len -1 \| 0 )` | Value into a static buffer (≤ 256 bytes — DRAM-tight hosts; longer values truncate) |
| `hkv-run` | `( k-addr k-len -- )` | Fetch; if non-empty: **clear the key, then evaluate the value as Forth** — at-most-once command execution. The generalized `boombox:cmd` pattern: hive commands are Forth phrases. |

`hkv-run` is what makes Worker/Beeper/Pet/Warrior one-liners: the Ruler leaves a
Forth phrase in the role's command key (`s" 1200 100 buzz" s" beeper:cmd" hkv-put$`
from any node, or `kv-set` on the ruler), and the role's next tick executes it.

## Versioning the format itself

The bundle format is versioned by `min_proto` (currently 1). Format-breaking changes (new required fields, alg changes that drop old support) bump this number; old nodes refuse new bundles cleanly with `BUNDLE_ERR_PROTO`.
