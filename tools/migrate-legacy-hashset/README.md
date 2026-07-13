# migrate-legacy-hashset

Migrates iotonekit-style colon-path HASH-SET records (UDM 0.9.1 legacy form)
to **UDM-MagNET v1.0** JSON. See [`specs/UDM-MagNET-v1.md`](../../specs/UDM-MagNET-v1.md)
§6.3 (target form), §6.4 (legacy form), §8 (rename rationale).

## Install

```bash
cd tools/migrate-legacy-hashset
npm install
```

## Usage

```bash
# Migrate one record file (kv format) to UDM-MagNET v1.0 JSON on stdout:
npx tsx migrate.ts examples/zigbee-onoff-switch.kv --pretty

# Warnings go to stderr; redirect them out if you only want the JSON:
npx tsx migrate.ts examples/zigbee-onoff-switch.kv --pretty 2>warnings.log >device.json
```

## What it does

| Legacy input | v1.0 output |
|---|---|
| `udm_oem` | renamed to `udm_vendor` (warning emitted) |
| `udm_chipset_details:udm_chipset_vendor` etc. | renamed to flat names (`vendor`, `frequency`, …) per spec §3.3 |
| `<n>:<attr>:<name>` and `<n>:<attr>:<name>:_` | grouped into `udm_endpoints_x[]` (preserved as a legacy extension) |
| `udm_capability_x` rows | folded into `udm_capabilities_x: [{ name, id, value }]` |
| `udm_version=0.9.1` | rewritten to `1.0`; original preserved as `udm_source_version_x` |
| `<key>:_` companions at top level | promoted to `<renamed_key>_value_x` |

The output is always pinned to `udm_version: "1.0"` and includes a `_comment_migration`
breadcrumb (an underscore-prefixed annotation per spec §2.2; ignored at runtime).

## What it does *not* do

- It does **not** re-model legacy endpoints as USM services. The `udm_endpoints_x`
  array preserves the structure for downstream review. Re-modeling is manual.
- It does **not** validate against UDM-MagNET v1.0 (the `validateManifest`
  validator lives in `prototype/d3-spatial/src/manifest/schema.ts` and operates on
  full dataspace manifests). Run the migrated record through that validator after
  composing it into a manifest.
- It does **not** read Redis directly. Export with `HGETALL` and feed the result
  in either iotonekit `K<n>=...` form or as a `key=value` per-line file.

## Input formats

**1. iotonekit K/V form** (paired numbered lines):

```
K1=udm_version
V1="0.9.1"
K2=udm_key
V2="iotone-proto-switch-1"
...
```

**2. Simple key=value form** (one entry per line, optional quotes, `#` / `//` comments):

```
udm_key=iotone-proto-switch-1
udm_oem="IoTone"
# capability rows still use legacy colon-path keys:
1:udm_capability_x:OnOff=0006
1:udm_capability_x:OnOff:_=1
```

## Tests

```bash
npm test
```

Covers: parsing both input forms, `oem`→`vendor` rename, chipset stutter renames,
endpoint grouping, `:_` invocation suffix handling, scalar/array count mismatch,
and the full iotonekit fixture round-trip.

## Limitations

- One device record per file. Multi-device migration: split inputs first.
- The `<n>:<attr>:<name>` pattern is heuristically interpreted as
  `endpoint_index N, attribute attr, named entity name`. Other historical patterns
  (e.g. attribute-name with embedded colons) will be preserved verbatim under
  `<attr>_<sanitized_name>_x` with a warning.
- No automatic round-trip back to legacy form is provided. The legacy form is a
  Zigbee-tinted encoding; the dotted-path form (spec §6.3) is the v1.0 canonical
  HASH-SET.
