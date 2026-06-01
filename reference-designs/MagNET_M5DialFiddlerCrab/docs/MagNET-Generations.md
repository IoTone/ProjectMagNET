# MagNET Generations & Lineage Gate

**Status**: Layer 1 (gen plumbing) and Layer 2 (CHALLENGE/RESPONSE puzzle) both implemented in 0.5.0-spore. Layer 2 ships **default-OFF** for backward compatibility — flip on via `1 lineage-auth` from the Dial Forth REPL or `--require-lineage-auth` on `fake_ruler.py`.

**Scope**: how MagNET firmware advertises its generation, how the hive decides "this biologic is one of us", and how the codebase evolves through mycology-themed lineages.

## Why three version axes

A Phase-4 biologic carries three orthogonal version concepts. Conflating them produces grief whenever any one of them moves.

| Axis    | Lives in                              | Bumps when                                           | Visible to peers as |
|---------|---------------------------------------|------------------------------------------------------|---------------------|
| `proto` | `CRAW_HIVE_PROTO_VERSION`             | HELLO/WELCOME/KV/ROLE_GRANT field schemas change     | mDNS TXT `ver=1`    |
| `gen`   | `include/magnet_gen.h`                | Firmware capabilities or NVS schemas evolve overall  | HELLO/WELCOME `gen` |
| `role`  | `craw_role_bundle` NVS slot           | A node accepts a new `ROLE_GRANT`                    | HELLO `role_requested` |

`proto` is the wire spec. `gen` is the firmware family. `role` is the runtime function. They move on different timelines and a peer that mismatches one may still cooperate on the others.

## Generation tag (Layer 1)

Defined once in [`include/magnet_gen.h`](../include/magnet_gen.h) and embedded by every project:

```
MAGNET_GEN_STR = "<MAJOR>.<MINOR>.<PATCH>-<lineage>"
```

Example for the current tree: `0.5.0-spore`.

### Bump rules

- **PATCH** — bug fixes only, no observable behavior change.
- **MINOR** — new optional features, new caps, new Forth words, new bundle-installable abilities.
- **MAJOR** — incompatible NVS schemas, removed Forth words, required new caps, *or* a new mycology lineage. A MAJOR bump always changes the lineage codename and adds a new entry to `magnet_lineages.c`.

PATCH and MINOR bumps stay backward compatible. A `0.5.x` ruler must accept a `0.4.y` joiner; a `0.4.y` ruler must accept a `0.5.x` joiner.

A MAJOR bump is the moment the hive's tribe instinct may say "you are not one of us" — see Layer 2.

### Mycology lineages

Each MAJOR family has a codename drawn from the mycelial life cycle. Old lineages remain valid forever (a `1.x-hyphae` ruler can still recognise a `0.9-spore` biologic — it just routes the join through the older puzzle key).

| MAJOR | Codename    | Stage of growth                          |
|-------|-------------|------------------------------------------|
| 0     | `spore`     | sporulation, the seed stage              |
| 1     | `hyphae`    | first network filaments                  |
| 2     | `mycelium`  | full underground network                 |
| 3     | `fruiting`  | visible reproductive body                |
| 4     | `sporocarp` | mature reproducer                        |

Adding a codename is a deliberate decision tied to a MAJOR bump, not a marketing rename.

### What carries the gen tag

- **HELLO** payload includes `"gen": "0.5.0-spore"` (set by `craw_hive_node.c` from `cfg.gen`).
- **WELCOME** payload echoes the ruler's gen back to the joiner (set by `craw_hive_ruler.c` from `cfg.gen`).
- The ruler's per-peer `session_t` stores the joiner's gen for inspection via `craw_hive_ruler_peer_gen()`.
- Each project's boot banner prints `MagNET gen <str>` so a serial-console glance is enough to confirm what's flashed.

### What does NOT yet act on gen

- The ruler does not currently reject a peer based on gen mismatch. v1 is observational only.
- The node does not currently capture the ruler's gen back from WELCOME (the puzzle layer below changes this).
- Role bundles do not yet enforce `min_gen` (planned for the bundle envelope schema).

## Lineage puzzle gate (Layer 2 — implemented, default-off)

> "It's up to the hive tribe to decide if a node can join. Sort of a 'you are not one of us' if it is too old or orthogonally related. Every join requires some puzzle to be solved, the solution only known in the DNA of the biologic. You keep old solutions."

The Layer 2 idea: each MAJOR lineage carries a 32-byte "DNA key" baked into firmware. The ruler issues a CHALLENGE during join, and the joiner must answer using the key for *some* lineage the ruler still recognises. A peer that doesn't know any of the keys the ruler keeps is — by construction — not one of us.

### State machine addition

Insert one round-trip between HELLO and WELCOME:

```
node ──HELLO──────────────►   ruler
node ◄─CHALLENGE──────────    ruler   (issued only if ruler intends to verify lineage)
node ──RESPONSE──────────►    ruler
node ◄─WELCOME (or REJECT)    ruler
```

If the ruler does not require lineage verification (e.g. dev mode, or `gen` field absent from HELLO), it skips CHALLENGE and proceeds straight to WELCOME — backward compatible.

### CHALLENGE (ruler → node)

```json
"payload": {
  "lineage":   "spore",                      // which DNA key to use
  "puzzle":    "<base64 16 bytes random>",   // ruler-chosen nonce
  "kdf":       "hmac-sha256",                // currently the only option
  "expires_in": 10                            // seconds; ruler closes the conn after
}
```

`lineage` lets a ruler that holds many lineage keys ask the joiner specifically which one to use — typically matching the joiner's own `gen` lineage from HELLO, but the ruler may also issue an "older lineage" challenge to test legacy keys.

### RESPONSE (node → ruler)

```json
"payload": {
  "lineage":  "spore",
  "answer":   "<hex HMAC-SHA256(dna_key, puzzle || node_id || ts)>"
}
```

The node looks up the requested lineage in its local `magnet_lineages.c` table. If the node was built from a tree that does not include that lineage's key (i.e. a future-only lineage compiled out), it sends `REJECT { reason: "lineage_unknown" }` and the ruler closes the connection.

### Ruler verification

```c
expected = HMAC-SHA256(dna_key[lineage], puzzle || node_id || ts)
if (constant_time_eq(expected, answer)) WELCOME(); else REJECT("lineage_auth");
```

The ruler holds an array of `(lineage_codename, dna_key[32])`. To accept "any biologic that knows *some* lineage", iterate; to enforce "must speak my current lineage", check only that one.

### Why HMAC over the puzzle, not just a static key

A static key proves "I have the value", not "I am freshly responding". Mixing the ruler-issued nonce, the joiner's id, and a timestamp into the HMAC makes RESPONSE replay-resistant: a captured RESPONSE for one `(node, ts, puzzle)` cannot be reused on a different connection.

### Where the keys live

`components/craw_hive/magnet_lineages.c` (planned). One file, hand-edited at each MAJOR bump:

```c
const magnet_lineage_t MAGNET_LINEAGES[] = {
    { "spore",      { 0xA0, 0x8F, 0x19, /* ... 32 bytes ... */ } },
    { "hyphae",     { /* ... */ } },          // added when MAGNET_GEN_MAJOR -> 1
    { "mycelium",   { /* ... */ } },          // added when -> 2
    /* etc */
    { NULL, {0} }
};
```

The lineage key is *not* the same as `CRAW_HIVE_DEV_SECRET`. The hive secret protects the wire (HMAC over every frame). The lineage key proves you are descended from the same firmware family. A leaked dev secret breaks wire authentication; a leaked lineage key only erodes the tribe filter for one MAJOR — and the next MAJOR rotates it.

### Compatibility matrix

|                   | ruler 0.x-spore | ruler 1.x-hyphae | ruler 2.x-mycelium |
|-------------------|-----------------|------------------|--------------------|
| node 0.x-spore    | ✅ direct        | ✅ via spore key  | ✅ via spore key    |
| node 1.x-hyphae   | ❌ (ruler doesn't know hyphae) | ✅ direct | ✅ via hyphae key |
| node 2.x-mycelium | ❌              | ❌               | ✅ direct           |

Forward compatibility is asymmetric on purpose: a newer ruler accepts older nodes (it carries their lineage keys), but an older ruler cannot accept a node from a future lineage. To onboard a new MAJOR you flash the rulers first.

### REJECT reasons (additions)

| Reason            | Meaning                                                          |
|-------------------|------------------------------------------------------------------|
| `lineage_unknown` | Joiner doesn't carry the lineage key the ruler asked for         |
| `lineage_auth`    | RESPONSE HMAC didn't verify under any accepted lineage           |
| `gen_too_old`     | Ruler's tribe policy refuses a `gen` below a configured floor    |

`gen_too_old` is policy-driven (per-deployment), not protocol-driven. The puzzle layer is identity; gen-floor is a tribe rule layered on top.

## Migration playbook — bumping MAJOR

When the codebase warrants a MAJOR (e.g. an NVS schema that older firmware can't read):

1. Bump `MAGNET_GEN_MAJOR` in `include/magnet_gen.h`. The lineage codename auto-flips via the `#if` ladder.
2. Generate a fresh 32-byte random DNA key for the new lineage:

   ```bash
   python -c 'import secrets; print(", ".join(f"0x{b:02X}" for b in secrets.token_bytes(32)))'
   ```

3. Append the new `(codename, key)` row to `MAGNET_LINEAGES[]` in `magnet_lineages.c`. **Do not delete or reorder existing rows.** Old biologics are still valid hive members and still authenticate via their lineage's key.
4. Update the compatibility-matrix row in this doc.
5. Bump `DEFAULT_RULER_GEN` in `scripts/fake_ruler.py`.
6. Flash the ruler(s) first, then the nodes. (Forward asymmetry — older ruler will reject new node otherwise.)
7. After all production rulers are upgraded, you *may* add a `gen_floor` config to evict pre-MAJOR biologics. Layer 2 is opt-in; Layer 1 stays observational forever.

## Implementation status (2026-05-01)

Layer 1 (observational gen):
- ✅ `include/magnet_gen.h` — single source of truth, mycology lineages
- ✅ `craw_hive_node_config_t.gen` + HELLO emits `gen`
- ✅ `craw_hive_ruler_config_t.gen` + WELCOME emits `gen`
- ✅ Ruler stores joiner gen per-session; `craw_hive_ruler_peer_gen()` getter
- ✅ Dial `ruler-status` Forth word lists peer gen column
- ✅ All five hive-participating firmware projects pass `MAGNET_GEN_STR` (Dial, Echo, Camera, Capsule, Atom Matrix)
- ✅ `scripts/fake_ruler.py` round-trips gen, exposes `--gen` CLI flag

Layer 2 (puzzle gate, default-off):
- ✅ `components/craw_hive/magnet_lineages.[ch]` — DNA-key table (spore key compiled in), HMAC helper, gen→codename extractor
- ✅ `CRAW_HIVE_MSG_CHALLENGE` / `CRAW_HIVE_MSG_RESPONSE` message types
- ✅ Ruler-side `lineage_gate()` runs between HELLO decode and WELCOME when `require_lineage_auth` is on
- ✅ Node-side transparent CHALLENGE handler in `session_attempt()`
- ✅ Runtime knob: `craw_hive_ruler_set_lineage_auth()` / `_get_lineage_auth()`
- ✅ Dial Forth word `N lineage-auth` (1=on, 0=off) — toggles without reflash
- ✅ `scripts/fake_ruler.py --require-lineage-auth` mirrors the firmware gate; `LINEAGE_KEYS` table mirrors `magnet_lineages.c`
- ✅ Reject reasons added: `lineage_unknown`, `lineage_auth`, `gen_too_old`
- ⏸ `gen_floor` numeric policy hook — designed, not implemented (the puzzle gate is the only enforcement so far)
- ⏸ Hyphae / mycelium / fruiting / sporocarp keys — added at MAJOR bump time; only `spore` is populated now
