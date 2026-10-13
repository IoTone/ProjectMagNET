# UDM-MagNET v1.0 / USM-MagNET v1.0

**Status:** draft
**Date:** 2026-04-30
**Editors:** ProjectMagNET maintainers
**Forked from:** [IoTone UDM 0.9.1](https://github.com/IoTone/IoToneSpec_UniversalDeviceMetadata) and [USM 0.9.1](https://github.com/IoTone/IoToneSpec_UniversalServiceMetadata) (Apache 2.0, 2014–2015). Upstream has been quiescent for ~10 years; this document is the authoritative reference for any ProjectMagNET artefact. Where this document conflicts with upstream, **this wins** for ProjectMagNET use.

---

## 1. Why fork

Three reasons:

1. **Upstream stalled.** No commits since 2015, JSON-Schema draft-03, internally inconsistent (the upstream USM example contains the typo `usm_sevices` and uses `usm_version_number`, which is not in the schema).
2. **Real divergences already exist.** Both `MagNET_M5DialFiddlerCrab` (Redis-Lua HASH-SET form) and `webxrofthings/d3-spatial` (JSON form in dataspace manifests) ship today with field names and value shapes that don't match upstream. Pretending we conform creates interop debt.
3. **JSON has no comments — and that hurt.** The original implementations used JSON for both spec docs and instance docs. Documenting *why* a field had a particular value, marking work-in-progress, or annotating exceptions all required out-of-band text. This spec fixes that explicitly (§2).

This document is the spec; instance documents conform to it; tooling (`d3-spatial/src/manifest/schema.ts`, the dial firmware's UDM loader, the Redis-Lua importer) implements it.

## 2. Authoring & documentation conventions

### 2.1 Three layers, three formats

| Layer | Format | Comments? | Audience |
|---|---|---|---|
| **Spec document** (this file) | Markdown | yes (markdown prose) | humans |
| **Authoring source** for instance docs | JSONC (JSON + `//` and `/* */` comments) | yes | maintainers editing device files |
| **Wire / canonical** form | JSON (RFC 8259) | no | runtime consumers |

A one-line stripper canonicalizes JSONC → JSON for the wire:

```bash
# Either:
npx strip-json-comments-cli < device.jsonc > device.json
# Or with jq (jq tolerates comments in input on most builds):
jq . device.jsonc > device.json
```

JSONC is a superset of JSON, so existing parsers that don't strip comments continue to work as long as documents on the wire are pre-stripped. No runtime parser change is required for v1.0.

### 2.2 Inline annotations inside JSON (when JSONC isn't an option)

Any JSON object MAY include keys whose names start with an underscore. Conforming readers **MUST** ignore them at runtime. Recommended names:

- `_comment` — free-form note about the enclosing object
- `_comment_<topic>` — multiple notes per object (e.g. `_comment_battery`, `_comment_known_issue`)
- `_doc` — link to external documentation, typically a URL

This keeps wire JSON valid while letting maintainers annotate on the spot.

```json
{
  "udm_key": "magnet-cam-80e4",
  "_comment": "AI-Thinker ESP32-CAM. NO-SOI errors fixed by lowering XCLK to 10 MHz.",
  "_doc": "https://github.com/iotone/ProjectMagNET/.../CAMERA_SETUP.md",
  "udm_model_name": "ESP32-CAM"
}
```

Underscore-prefixed keys are reserved for this purpose. **Do not** introduce data fields whose names start with `_`.

### 2.3 Naming

- snake_case throughout.
- Standard fields use the `udm_` / `usm_` prefix.
- Project extensions use a trailing `_x` (e.g. `udm_spatial_anchor_x`). When a field is promoted into the spec, the `_x` is dropped.
- Inside nested objects (`udm_chipset_details`, `udm_memory_volatile`), subfields are unprefixed (`vendor`, `type`, `frequency`). This is a deliberate divergence from upstream, which double-prefixes (`udm_chipset_vendor`); see §8.

### 2.4 Identity

Three identifiers, three jobs:

| Field | Scope | Stable across | Mandatory | Example |
|---|---|---|---|---|
| `udm_key` / `usm_key` | Document-scoped | one document | **yes** | `magnet-cam-80e4` |
| `udm_uuid` / `usm_uuid` | Global | all documents, all time | recommended | `urn:device:esp32-cam:80e4` |
| `udm_serialno` | Per physical instance | one device's lifetime | optional | `AT1234567` |

**Refs inside a document target `udm_key` / `usm_key`.** Cross-document refs use `udm_uuid` / `usm_uuid`. Mark `deviceRef` and `serviceRef` in dataspace manifests are doc-scoped, so they target `_key`.

UUID format: any URI conforming to RFC 3986, including RFC 4122 UUIDs (`urn:uuid:...`) and custom URN schemes (`urn:device:...`). Lock to `urn:` URIs for new documents.

### 2.5 Versioning (three things, don't conflate)

- `udm_version` / `usm_version` — **spec version** the document conforms to. For documents using this spec: `"1.0"`.
- `udm_doc_version` / `usm_doc_version` — **document version** managed by the document's author. Semver string (`"2.3.1"`). Optional but recommended.
- `udm_model_rev`, `udm_fw_version_x` — **entity version** (hardware revision, firmware version). Per-device.

### 2.6 Extensions

Anything not in this spec gets `_x`. Examples we already need:

- `udm_spatial_anchor_x` — `{ x, y, z }` location relative to dataspace anchor (§9.1).
- `udm_fw_version_x` — running firmware version string.
- `usm_polling_x` — `{ interval_seconds }` for polled characteristics.
- `usm_subscription_x` — `{ transport: "sse" | "ws" | "ble-notify" | "mqtt", topic? }` for push.

Extensions promoted into v1.1 lose the `_x`. Maintainers SHOULD propose extensions back as PRs against this file before broad use.

---

## 3. UDM-MagNET core

### 3.1 Document shape

```jsonc
{
  "udm_version": "1.0",
  "udm_doc_version": "0.1.0",                 // optional
  "udm_schema_name": "magnet-livingroom",     // optional, free-form
  "udm_devices": [ /* udm_device objects */ ]
}
```

When UDM is embedded inside a dataspace manifest (the d3-spatial case), `udm_devices` is an array at the dataspace root. The wrapper-level `udm_version` field MUST be present once per document, on the outermost UDM container.

### 3.2 Per-device fields

| Field | Type | Required | Notes |
|---|---|---|---|
| `udm_key` | string | yes | Document-scoped reference target. Stable, snake-or-kebab-case. |
| `udm_uuid` | string (URI) | rec | Global identity. URN preferred. |
| `udm_serialno` | string | no | Physical-instance serial. |
| `udm_model_name` | string | rec | Manufacturer's model name. |
| `udm_model_number` | string | no | Manufacturer's model number/SKU. |
| `udm_model_rev` | string | no | Hardware revision. |
| `udm_mktg_name` | string | no | User-friendly product name. |
| `udm_vendor` | string | rec | Manufacturer name. (`udm_oem` is a deprecated alias; readers SHOULD accept either, writers SHOULD emit `udm_vendor`.) |
| `udm_mfg_origin` | string | no | Country / facility of manufacture. |
| `udm_mfg_date` | string (ISO 8601) | no | Manufacturing date. |
| `udm_type` | string | rec | High-level kind: `camera`, `compute-node`, `sensor`, `actuator`, `display`, `gateway`, `wearable`, ... |
| `udm_class` | string[] | no | Tags for cross-cutting categories: `["camera", "iot", "wifi"]`. **Array, not string** (divergence from upstream JSON-Schema, which says string; spec examples already use lists). |
| `udm_capabilities` | string[] | no | Short capability tags: `["mjpeg-stream", "single-frame-capture", "ov2640"]`. Free-form. Composers MAY define their own conventions; matching is exact. |
| `udm_tags` | string[] | no | Deployment / topology tags: `["room", "lan-only", "no-tls"]`. |
| `udm_chipset_details` | object | no | See §3.3. |
| `udm_memory_volatile` | object \| string | no | Either `{ size, type? }` or a free-form descriptor string. Object form preferred for tooling. |
| `udm_memory_non_volatile` | object \| string | no | Same as above. |
| `udm_sensors` | string[] | no | Sensor identifiers: `["camera-ov2640", "esp32c3-internal-temp"]`. **Array of strings** (divergence from upstream's `{name: "1"}` map; the upstream form has duplicate-key issues in its own examples). |
| `udm_displays` | object[] | no | Each: `{ resolution, size, type? }`. |
| `udm_io_ports` | string[] | no | Free-form: `["usb-c", "qwiic", "grove"]`. |
| `udm_battery_details` | object | no | `{ chemistry, capacity_mah?, removable? }`. |
| `udm_power_input` | string \| object | no | E.g. `"5V/2A USB-C"` or structured. |
| `udm_power_outputs` | string[] \| object[] | no | |
| `udm_network_interfaces` | object[] | no | Each: `{ kind: "wifi" \| "ethernet" \| "ble" \| "thread" \| "zigbee" \| "lora", details? }`. |
| `udm_services_link` | string[] | no | Array of `usm_key` values referenced in the same document. For external services, use a URI. See §5. |
| `udm_os` | string | no | OS / runtime: `"esp-idf 5.2"`, `"zephyr 3.6"`, `"arduino"`, `"linux"`. |
| `udm_os_version` | string | no | |
| `udm_fw_version_x` | string | no | Currently-running firmware version. *Extension; candidate for v1.1 promotion.* |
| `udm_spatial_anchor_x` | object | no | `{ x, y, z }` metres relative to the dataspace anchor. *Extension.* |

Implementations MUST ignore unknown fields (forward-compat).

### 3.3 `udm_chipset_details`

```jsonc
{
  "vendor": "Espressif",       // upstream says `udm_chipset_vendor`; we drop the stutter
  "type": "ESP32",
  "instruction_set": "xtensa", // upstream `udm_chipset_inst_set`
  "frequency": "240MHz",       // upstream `udm_cpu_max_frequency`
  "core_count": 2              // upstream `udm_cpu_number_of_cores`
}
```

All subfields optional. See §8 for divergence rationale.

---

## 4. USM-MagNET core

### 4.1 Document shape

```jsonc
{
  "usm_version": "1.0",
  "usm_serviceset_name": "kords-livingroom",  // optional
  "usm_services": [ /* usm_service objects */ ]
}
```

### 4.2 Per-service fields

| Field | Type | Required | Notes |
|---|---|---|---|
| `usm_key` | string | yes | Document-scoped reference target. |
| `usm_uuid` | string (URI) | rec | Global identity. |
| `usm_service_name` | string | yes | Human-readable name. |
| `usm_type` | string | rec | High-level kind: `video`, `image`, `sensor`, `timeseries`, `command`, `event-stream`. |
| `usm_class` | string[] | no | Tags. |
| `usm_service_version` | string | no | Service implementation version (semver). |
| `usm_vendor` | string | no | |
| `usm_service_endpoint` | object | yes | See §4.3. **HTTP-verb-keyed canonical**, divergence from upstream's RPC-style. |
| `usm_capabilities` | object | no | Free-form constraint/limit object: `{ "framerate_hz": [5, 10], "resolution": "VGA 640x480" }`. |
| `characteristics` | object[] | no | See §4.4. |
| `usm_tags` | string[] | no | |
| `usm_polling_x` | object | no | `{ interval_seconds }` — extension for polled sensors. |
| `usm_subscription_x` | object | no | `{ transport, topic? }` — extension for push. |
| `usm_rpc_endpoint_x` | object | no | RPC-style endpoint per upstream USM 0.9.1, when really needed. Extension. |

### 4.3 `usm_service_endpoint` (HTTP form)

```jsonc
{
  "GET":    { "url": "http://magnet-cam-80e4.local/stream", "content-type": "multipart/x-mixed-replace" },
  "POST":   { "url": "...", "content-type": "application/json", "request-body": "..." }, // optional descriptors
  "DELETE": { "url": "..." }
}
```

Each verb is optional; omit verbs the service doesn't support. Keys are HTTP method names in upper-case.

For non-HTTP services (BLE GATT, MQTT, CoAP), use `usm_subscription_x` (push) or `usm_rpc_endpoint_x` (request/response with explicit transport). Future versions MAY define `usm_ble_endpoint_x`, `usm_mqtt_endpoint_x`, etc.

### 4.4 `characteristics`

A characteristic describes a single read/write/notify item the service exposes (mirrors BLE GATT terminology, which is the project's most-cited transport).

```jsonc
{
  "usm_characteristic_id": "celsius",
  "usm_characteristic_format": "number",
  "usm_characteristic_constraints": { "min": -40, "max": 125 },
  "usm_characteristic_access_x": ["read", "notify"]   // extension; ['read','write','notify']
}
```

`usm_characteristic_format` SHOULD be one of: `number`, `integer`, `boolean`, `string`, `iso8601`, `uri`, `image/jpeg`, `image/png`, `array<{t,v}>` (time-series). Implementations MAY accept others.

---

## 5. Linking devices ↔ services

`udm_devices[].udm_services_link` is an array. Two interpretations, decided by string content:

- **Bare key** (`"cam-stream"`) — refers to a service in the same document by `usm_key`.
- **URI** (`"https://other-host/services/cam-stream.json"` or `"urn:service:..."`) — external reference.

Resolvers MUST try `usm_key` first when the value is a bare key; fall through to URI resolution otherwise.

For the inverse direction (service → device), there is no required field. If needed, services MAY include `usm_device_link_x: "<udm_key | URI>"`.

In dataspace manifests, mark-level `deviceRef` / `serviceRef` always target document-scoped `udm_key` / `usm_key`. URI refs in marks are not allowed in v1.0.

---

## 6. Wire forms

### 6.1 Canonical: JSON

JSON (RFC 8259), UTF-8, no BOM. Underscore-prefixed keys MAY appear (§2.2) and are ignored at runtime. Object key order is not significant. Numeric values use JSON number; string values use JSON string.

### 6.2 Authoring: JSONC

JSONC (JSON with `//` line comments and `/* block */` comments) is RECOMMENDED for files maintained by hand. Project tooling SHOULD provide a build step that strips comments to produce wire JSON. Files committed to source control MAY be in either form; consumers MAY require canonical JSON at runtime.

### 6.3 HASH-SET projection (for Redis / BLE / constrained transport)

A flat key-value projection of canonical JSON. Used by:

- The dial's Redis-Lua importer (currently uses an iotonekit-style colon-path encoding; see §6.4 migration note).
- BLE GATT advertisements where the entire JSON document doesn't fit a single attribute.
- MQTT topic flattening.

#### Path grammar

```
PATH       = SEGMENT ( "." SEGMENT )*
SEGMENT    = FIELD_NAME | INDEX
FIELD_NAME = snake_case_identifier         ; MUST NOT contain "."
INDEX      = unsigned_decimal              ; 0-based
```

#### Storage convention

Each entity is stored as a single Redis hash keyed `udm:device:<udm_key>` or `usm:service:<usm_key>`. Membership of the dataspace's device/service set is tracked in `udm:devices:<dataspace_key>` and `usm:services:<dataspace_key>` (Redis sets of `_key` values).

#### Type mapping

| JSON | HASH-SET value |
|---|---|
| string | the string, UTF-8, unquoted |
| number | base-10 numeric string (preserve precision; `42`, `3.14`, `-0.001`) |
| boolean | `true` / `false` |
| null | empty string OR field absent (project decision; **prefer absent**) |
| array of primitives | `field.0`, `field.1`, ... each a separate hash field |
| array of objects | `field.0.subfield`, `field.1.subfield`, ... |
| nested object | dotted path: `field.subfield` |

Underscore-prefixed annotation keys (`_comment`, `_doc`) MUST be omitted in the HASH-SET projection.

#### Example

JSON:
```json
{
  "udm_key": "magnet-cam-80e4",
  "udm_class": ["camera", "iot", "wifi"],
  "udm_chipset_details": { "vendor": "Espressif", "type": "ESP32" },
  "udm_spatial_anchor_x": { "x": 0.0, "y": 1.6, "z": -1.2 }
}
```

HASH-SET (Redis hash `udm:device:magnet-cam-80e4`):
```
udm_key                       = magnet-cam-80e4
udm_class.0                   = camera
udm_class.1                   = iot
udm_class.2                   = wifi
udm_chipset_details.vendor    = Espressif
udm_chipset_details.type      = ESP32
udm_spatial_anchor_x.x        = 0.0
udm_spatial_anchor_x.y        = 1.6
udm_spatial_anchor_x.z        = -1.2
```

### 6.4 Legacy iotonekit colon-path form

The iotonekit modelling writeup (`docs/iotonekit_modelling_with_udm.txt`) defines a different HASH-SET form using colon-paths and packed-key/value pairs (e.g. `K9="1:udm_type:OnOff Switch" V9="0103"`). This is **legacy-format-1** for migration purposes only. New ProjectMagNET implementations MUST use the §6.3 dotted-path form. A migration script SHOULD live in `tools/migrate-legacy-hashset/` when needed; out of scope for v1.0.

---

## 7. Validation

A document is *valid UDM-MagNET v1.0* iff:

1. `udm_version` is `"1.0"` at the outermost UDM container.
2. Each `udm_device` has a non-empty string `udm_key`.
3. `udm_key` values are unique within the document.
4. Every value in `udm_services_link` either matches a `usm_key` in the same document or is a syntactically valid URI.
5. Mark-level `deviceRef` / `serviceRef` (in d3-spatial dataspace manifests) resolve to a `udm_key` / `usm_key` in the same document.
6. Required fields per §3.2 / §4.2 are present.

Validators SHOULD also surface warnings for:

- `udm_oem` set without `udm_vendor` (deprecated alias).
- Subfields named `udm_chipset_vendor` etc. inside `udm_chipset_details` (upstream stutter form; rename to `vendor`).
- Missing `udm_uuid` / `usm_uuid` on entities likely to be referenced cross-document.

The d3-spatial reference implementation lives in `prototype/d3-spatial/src/manifest/schema.ts` (`validateManifest`); extend it as v1.0 lands.

---

## 8. Divergence ledger from upstream 0.9.1

| Upstream 0.9.1 | UDM-MagNET v1.0 | Why |
|---|---|---|
| `udm_oem` | `udm_vendor` (canonical), `udm_oem` accepted as alias | Two synonyms in upstream with no defined difference; pick one. |
| `udm_chipset_details.udm_chipset_vendor` | `udm_chipset_details.vendor` | Drop the stutter inside an already-prefixed object. Easier to read, easier in HASH-SET projection. |
| `udm_chipset_details.udm_chipset_type` | `udm_chipset_details.type` | Same. |
| `udm_chipset_details.udm_chipset_inst_set` | `udm_chipset_details.instruction_set` | Same + spell out the abbreviation. |
| `udm_chipset_details.udm_cpu_max_frequency` | `udm_chipset_details.frequency` | Same. |
| `udm_chipset_details.udm_cpu_number_of_cores` | `udm_chipset_details.core_count` | Same. |
| `udm_class` declared `string` in JSON-Schema | `udm_class: string[]` | Upstream's own examples already use arrays; the schema is wrong. |
| `udm_capabilities` declared `string` | `udm_capabilities: string[]` | Same. |
| `udm_sensors: { name: "1", ... }` | `udm_sensors: string[]` | Upstream form has duplicate-key issues in its own examples; array is unambiguous. |
| `usm_service_endpoint: { op: { verb: [resource, params, callback] } }` (RPC-style) | `usm_service_endpoint: { GET: { url, content-type? }, ... }` (HTTP-verb-keyed) | Upstream baked in a JS-callback model that doesn't fit raw HTTP / streaming. Old form available as `usm_rpc_endpoint_x` extension when needed. |
| No `_comment` / `_doc` convention | Underscore-prefixed annotation keys reserved (§2.2) | Solves the "JSON has no comments" pain. |
| No JSONC authoring story | JSONC RECOMMENDED for source files (§6.2) | Same. |
| No HASH-SET path grammar (informal in iotonekit writeup) | Dotted-path grammar (§6.3); legacy colon-path form documented for migration only (§6.4) | Two implementations existed; pick one going forward. |
| No `udm_doc_version` / `usm_doc_version` | Added | Conflated three versioning concepts before; separate now. |
| No `udm_spatial_anchor` field | `udm_spatial_anchor_x` extension | Required for d3-spatial; promoted candidate for v1.1. |

---

## 9. Worked example: UC2 living-room dataspace

### 9.1 JSONC source (authoring form)

```jsonc
// kords-livingroom — UC2 dataspace
// One ESP32-CAM, one M5StampC3U with internal die-temp sensor.
{
  "udm_version": "1.0",
  "udm_schema_name": "kords-livingroom",
  "udm_devices": [
    {
      "udm_key":   "magnet-cam-80e4",
      "udm_uuid":  "urn:device:esp32-cam:80e4",
      "_comment":  "AI-Thinker ESP32-CAM. Lower XCLK to 10 MHz to fix NO-SOI errors.",
      "udm_model_name":   "ESP32-CAM",
      "udm_model_number": "AI-Thinker",
      "udm_vendor":       "AI-Thinker",
      "udm_mktg_name":    "Magnet Cam",
      "udm_type":         "camera",
      "udm_class":        ["camera", "iot", "wifi"],
      "udm_capabilities": ["mjpeg-stream", "single-frame-capture", "ov2640"],
      "udm_chipset_details": {
        "vendor": "Espressif", "type": "ESP32",
        "frequency": "240MHz", "core_count": 2
      },
      "udm_memory_volatile":     { "size": "520KB", "type": "SRAM", "extras": "+ 4MB PSRAM" },
      "udm_memory_non_volatile": { "size": "4MB",   "type": "flash" },
      "udm_sensors":         ["camera-ov2640"],
      "udm_services_link":   ["cam-stream", "cam-capture"],
      "udm_tags":            ["room", "lan-only", "no-tls"],
      "udm_spatial_anchor_x": { "x": 0.0, "y": 1.6, "z": -1.2 }
    },
    {
      "udm_key":   "magnet-stamp-c3u-a1b2",
      "udm_uuid":  "urn:device:m5stamp-c3u:a1b2",
      "_comment_accuracy": "Internal die sensor; ±5–10 °C absolute, stable for relative.",
      "udm_model_name": "M5StampC3U",
      "udm_vendor":     "M5Stack",
      "udm_mktg_name":  "Stamp C3U",
      "udm_type":       "compute-node",
      "udm_class":      ["mcu", "iot", "wifi"],
      "udm_capabilities": ["wifi-station", "ble", "internal-temp-sensor", "json-http", "forth-repl"],
      "udm_chipset_details": {
        "vendor": "Espressif", "type": "ESP32-C3",
        "frequency": "160MHz", "core_count": 1
      },
      "udm_memory_volatile":     { "size": "400KB", "type": "SRAM" },
      "udm_memory_non_volatile": { "size": "4MB",   "type": "flash" },
      "udm_sensors":        ["esp32c3-internal-temp"],
      "udm_services_link":  ["temp-current", "temp-history"],
      "udm_tags":           ["room", "lan-only"],
      "udm_spatial_anchor_x": { "x": -0.6, "y": 1.0, "z": -0.8 }
    }
  ],
  "usm_services": [
    {
      "usm_key":     "cam-stream",
      "usm_uuid":    "urn:service:mjpeg:cam-80e4",
      "usm_service_name": "Camera live MJPEG stream",
      "usm_type":    "video",
      "usm_class":   ["video", "mjpeg", "live"],
      "usm_service_endpoint": {
        "GET": { "url": "http://magnet-cam-80e4.local/stream", "content-type": "multipart/x-mixed-replace" }
      },
      "usm_service_version": "1.0",
      "usm_capabilities": { "framerate_hz": [5, 10], "resolution": "VGA 640x480" },
      "characteristics": [
        { "usm_characteristic_id": "frame", "usm_characteristic_format": "image/jpeg" }
      ],
      "usm_tags": ["lan-only", "live"]
    },
    {
      "usm_key":     "temp-current",
      "usm_uuid":    "urn:service:temperature:stamp-a1b2",
      "usm_service_name": "Internal temperature (current)",
      "usm_type":    "sensor",
      "usm_class":   ["sensor", "temperature", "read-only"],
      "usm_service_endpoint": {
        "GET": { "url": "http://magnet-stamp-c3u-a1b2.local/temperature", "content-type": "application/json" }
      },
      "usm_polling_x": { "interval_seconds": 2 },
      "characteristics": [
        { "usm_characteristic_id": "celsius",   "usm_characteristic_format": "number",   "usm_characteristic_constraints": { "min": -40, "max": 125 } },
        { "usm_characteristic_id": "timestamp", "usm_characteristic_format": "iso8601" }
      ],
      "usm_tags": ["live", "low-accuracy", "esp32c3-internal"]
    }
    // … temp-history and cam-capture elided for brevity
  ]
}
```

### 9.2 HASH-SET projection (Redis form)

Two device hashes, two service hashes, two membership sets:

```
SADD udm:devices:kords-livingroom magnet-cam-80e4 magnet-stamp-c3u-a1b2
SADD usm:services:kords-livingroom cam-stream temp-current

HSET udm:device:magnet-cam-80e4
  udm_key                       magnet-cam-80e4
  udm_uuid                      urn:device:esp32-cam:80e4
  udm_model_name                ESP32-CAM
  udm_vendor                    AI-Thinker
  udm_type                      camera
  udm_class.0                   camera
  udm_class.1                   iot
  udm_class.2                   wifi
  udm_capabilities.0            mjpeg-stream
  udm_capabilities.1            single-frame-capture
  udm_capabilities.2            ov2640
  udm_chipset_details.vendor    Espressif
  udm_chipset_details.type      ESP32
  udm_chipset_details.frequency 240MHz
  udm_chipset_details.core_count 2
  udm_memory_volatile.size      520KB
  udm_memory_volatile.type      SRAM
  udm_memory_volatile.extras    + 4MB PSRAM
  udm_services_link.0           cam-stream
  udm_services_link.1           cam-capture
  udm_tags.0                    room
  udm_tags.1                    lan-only
  udm_tags.2                    no-tls
  udm_spatial_anchor_x.x        0.0
  udm_spatial_anchor_x.y        1.6
  udm_spatial_anchor_x.z        -1.2

HSET usm:service:cam-stream
  usm_key                                    cam-stream
  usm_uuid                                   urn:service:mjpeg:cam-80e4
  usm_service_name                           Camera live MJPEG stream
  usm_type                                   video
  usm_class.0                                video
  usm_class.1                                mjpeg
  usm_class.2                                live
  usm_service_endpoint.GET.url               http://magnet-cam-80e4.local/stream
  usm_service_endpoint.GET.content-type      multipart/x-mixed-replace
  usm_service_version                        1.0
  usm_capabilities.framerate_hz.0            5
  usm_capabilities.framerate_hz.1            10
  usm_capabilities.resolution                VGA 640x480
  characteristics.0.usm_characteristic_id    frame
  characteristics.0.usm_characteristic_format image/jpeg
  usm_tags.0                                 lan-only
  usm_tags.1                                 live
```

`_comment` / `_comment_accuracy` from the source are dropped in the projection.

---

## 10. Open questions (for v1.1 discussion)

1. **Promote `udm_spatial_anchor_x` and `udm_fw_version_x`** out of extensions? Both are in active use.
2. **Add `udm_health` / `usm_health`**: simple `{ status: "ok" \| "degraded" \| "down", since? }` — would unify the dial's hive-status field with UDM.
3. **Time-series characteristic format**: `array<{t,v}>` is informal; consider a dedicated `usm_timeseries_x: { sample_field, time_field, time_unit }` descriptor.
4. **Internationalisation**: `udm_mktg_name`, `usm_service_name` are user-facing. Add a `<field>_i18n` companion shape?
5. **Security envelope**: signed UDM/USM documents (JOSE / COSE)? Currently out of scope; revisit when devices register over BLE without prior trust.
6. **MQTT / CoAP endpoint shapes**: `usm_mqtt_endpoint_x`, `usm_coap_endpoint_x` — define properly when first non-HTTP service ships.
7. **Lifecycle of `udm_serialno`** when devices are reflashed and assigned new identities (factory-reset story).
8. **BLE GAP Appearance per node type (UX during provisioning).** Hive nodes advertise over BLE for WiFi provisioning, and a generic-device icon in scanners (nRF Connect, LightBlue) is poor UX during bring-up — a meaningful icon helps the installer pick the right node when several are advertising. `craw_ble_provision_config_t` now carries an `appearance` (BT SIG 16-bit Appearance, AD type `0x19` + GAP characteristic `0x2A01`); when non-zero the value goes into both the advertisement and the post-connect GAP service. **Set per node type:**
   - `XIAO_ESP32C3_IOT_LIGHTING` — `0x0580` "Generic Light Fixtures" (category `0x016`). **Wired 2026-05-19.**
   - `MagNET_ReSpeaker_Boombox` — `0x0440` "Generic Audio Source" (audio category `0x011`). **Wired 2026-05-19.**
   - `M5_Hive_Cam` — camera / imaging appearance (likely a "Generic Camera"/"Webcam"-class value; verify exact hex in the BT SIG Assigned Numbers PDF). **TODO.**
   - `MagNET_Vitals_E4TH` — heart-rate / fitness appearance ("Generic Heart Rate Sensor" / "Heart Rate Belt"-class value, category `0x00D`, e.g. `0x0341`). **TODO.**

   Verify the exact subcategory hex against the current [Bluetooth SIG Assigned Numbers PDF](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Assigned_Numbers/out/en/Assigned_Numbers.pdf) before shipping a non-Generic value; categories are stable, subcategory numbers occasionally shift. Each reference design vendors its own copy of `components/craw_ble_provision/` — to roll this out, sync the updated `.h`/`.c` from `XIAO_ESP32C3_IOT_LIGHTING/components/craw_ble_provision/` into the target node's `components/craw_ble_provision/`, then set `prov_cfg.appearance = 0xNNNN` in that node's `main.c`.
9. **Standard provisioning UX language for hive nodes (visual feedback during bring-up).** Adoption: `XIAO_ESP32C3_IOT_LIGHTING` (uses the strip itself as the indicator), wired 2026-05-19. Standardize the same color/animation vocabulary across single-LED nodes (M5_Hive_Cam, MagNET_ReSpeaker_Boombox, MagNET_Vitals_E4TH, …) so an installer learns the meaning once and reads it on any node. Mapping:

   | Provisioning state | Visual | Notes |
   |---|---|---|
   | Unconfigured (`IDLE`, no creds, advertising) | **orange flash** ~1 Hz | invites BLE provisioning |
   | Connecting (`CREDS_RECEIVED` / `COMMIT_REQUESTED` / `CONNECTING`) | **orange flash** ~3 Hz | transient — faster cadence distinguishes from idle |
   | Connected (`CONNECTED`) | **green breathing** ~0.4 Hz | sinusoidal, low-amplitude, calm |
   | Failed (`FAILED`) | **red strobe** ~8 Hz | demands attention |
   | BLE characteristic write (transient ack on any state) | **white pulse** ~250 ms | one-shot confirmation that the node accepted the BLE packet |

   For single-LED nodes the same language belongs in `craw_status_led` as new modes: `CRAW_LED_WIFI_UNCONFIGURED`, `CRAW_LED_WIFI_CONNECTING`, `CRAW_LED_WIFI_CONNECTED`, `CRAW_LED_WIFI_FAILED`, plus a one-shot `craw_status_led_pulse_ack()` triggered by `craw_ble_provision` callbacks. (The existing `CRAW_LED_WIFI_OFFLINE` mode partly overlaps `WIFI_UNCONFIGURED`/`FAILED` — fold or rename as part of this work.) Each reference design vendors its own copy of `components/craw_status_led/`, so rolling this out means syncing the updated component + wiring the new modes into each node's WiFi/BLE callbacks. The lighting node's `render_status()` (in `XIAO_ESP32C3_IOT_LIGHTING/src/main.c`) is the canonical reference for the cadences and easing math.

---

## 11. Implementation status

| Implementation | Path | Conformance | Notes |
|---|---|---|---|
| d3-spatial dataspace manifest types | `webxrofthings/prototype/d3-spatial/src/manifest/schema.ts` | **v1.0 (2026-04-30)** | `udm_spatial_anchor_x` extension; `udm_oem` accepted as deprecated alias; `udm_doc_version` added. |
| d3-spatial validator | `…/schema.ts` `validateManifest` | **v1.0 (2026-04-30)** | Implements §7: `_key` uniqueness, `deviceRef`/`serviceRef` resolution, deprecated-alias warning, `udm_chipset_*` stutter warning. Tolerates underscore-prefixed annotations per §2.2. |
| UC2 example manifest | `…/examples/uc2-room.json` | **v1.0 (2026-04-30)** | `udm_version: "1.0"`, `udm_spatial_anchor_x`. |
| Legacy HASH-SET migration tool | `tools/migrate-legacy-hashset/` | **v1.0 (2026-05-01)** | Reads iotonekit `K<n>=...` / `key=value` input, emits v1.0 JSON. 13 tests, fixture-validated against the iotonekit OnOff Switch example. |
| Dial firmware Redis-Lua importer | `MagNET_M5DialFiddlerCrab/...` (TBD) | uses §6.4 legacy form | Migrate to §6.3 dotted-path; the migration tool above produces a v1.0 device record from a Redis `HGETALL` dump. |
| iotonekit modelling writeup | `MagNET_M5DialFiddlerCrab/docs/iotonekit_modelling_with_udm.txt` | upstream-aligned, pre-fork | Mark superseded; link to this spec. |

---

*This document is dual-licensed under Apache 2.0 (matching upstream) and CC BY 4.0 for the prose. Code examples are public domain.*
