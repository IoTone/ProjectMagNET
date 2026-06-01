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
| `sig_alg` | string | Signature algorithm. v1 supports `hmac-sha256`. v2 will add `ed25519`. |
| `sig` | hex string | Signature over the canonical signing input (below). Length depends on alg. |
| `src_b64` | base64 string | Forth source code, base64-encoded. Decoded length ≤ 4096 bytes for v1 (single-frame KV value cap). |

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
sig = Ed25519-Sign(author_privkey, signing_input)  # 64 bytes hex-encoded
```

## Trust model

### v1 — HMAC with the hive secret

Bundles are signed with the same 32-byte `CRAW_HIVE_DEV_SECRET` used for hive-protocol HMACs. **This means anyone who can verify can also sign** — the model is "nodes trust holders of the shared secret to publish bundles." Adequate for development and demo deployments where all hardware ships with the same key. Inadequate for production: a leak of one node's firmware leaks the publisher key.

### v2 (planned) — Ed25519 with per-author public keys

Each `author` has a 32-byte public key baked into the node's firmware via `components/craw_role_bundle/keys.h`. Authors hold private keys offline. Adding a new trusted author requires reflash. This matches the threat model where individual nodes can be physically compromised but firmware integrity is preserved (signed boot, etc.).

The `sig_alg` field exists from v1 specifically so a single node can support both schemes during the migration: receive v1 bundles signed with HMAC, and v2 bundles signed with Ed25519, choosing the verification path at runtime.

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
| `forth_eval_n(decoded_src, decoded_len)` | `BUNDLE_ERR_EVAL` | Forth syntax error or runtime fault |
| Persist envelope to NVS | `BUNDLE_ERR_NVS` | NVS write fails (rare; bundle is still active in RAM) |

On success, the bundle's Forth words are registered in the global vocabulary and its top-level body has been executed once. The node persists `name`, `version`, and the full envelope so the same role auto-resumes on next boot without re-fetching.

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
```

The output `spy.json` is ready to either:
- POST to a Scribe via `KV_PUT key=bundle:spy`, or
- Embed in a Ruler firmware as a bootstrap fallback (Step 3+).

## Forth role conventions

A role bundle's Forth source is just regular ESPIDFORTH. Conventional structure:

```forth
\ Role: spy v1.0.0 — periodic camera snapshot loop.
\ Calls FFI words registered by the host firmware.

: spy-snap-once   cam-snap drop ;
: spy-loop        begin spy-snap-once 5000 ms again ;

\ The bundle is evaluated once at install time. To run a loop, either:
\   - schedule it as a forth task (when supported)
\   - or define the words and let the host firmware invoke them on a timer.
spy-snap-once   \ one-shot capture as install-time confirmation
```

Bundles SHOULD avoid blocking forever at top level — the install task is shared and a runaway bundle blocks subsequent installs. Either define words and return, or use cooperative yields.

## Versioning the format itself

The bundle format is versioned by `min_proto` (currently 1). Format-breaking changes (new required fields, alg changes that drop old support) bump this number; old nodes refuse new bundles cleanly with `BUNDLE_ERR_PROTO`.
