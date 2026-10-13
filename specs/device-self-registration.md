# Device self-registration — design proposal

**Status:** Draft. Not yet implemented.
**Author:** dkords@gmail.com
**Companion to:** PROPOSAL.md V1.5 (live service discovery), R24/R32 (device-side join), UDM-MagNET-v1.md, USM (manifest service refs).
**Why now:** the Vite dev-proxy currently encodes each device's host as an `*_HOST` env var (`CAMERA_HOST`, `VITALS_HOST`, `ENV_HOST`, `BOOMBOX_HOST`, `LIGHTING_HOST`, `IMU_HOST`). Every DHCP renewal or new MAC4 suffix means editing `vite.config.ts` or restarting `npm run dev` with a new env. Devices already know their own IP — they should announce it.

---

## Goals

1. **Remove `*_HOST` env vars from the dev workflow.** The Vite proxy should resolve device hosts at runtime from a registry.
2. **Make the manifest device-instance-agnostic.** A manifest cites `udm_uuid` or a capability tag; the registry maps to the actual device's hostname + IP at load time.
3. **Survive device IP churn.** When a device's DHCP lease renews, the dataspace picks up the new IP on the next manifest reload (or on the next periodic refresh — see §6).
4. **Stay backward-compatible.** Env vars + hardcoded mDNS hosts continue to work as fallback for unprovisioned devices and offline-LAN testing.
5. **Anchor to existing infrastructure.** Reuse the `mock-join-server` (already on port 3001, serves `/api/v1/*`) as the registry host. Reuse the existing BLE-provisioning + hive scribe flows for the device side. No new processes.

## Non-goals (for v1)

- A real authoritative context engine. The registry is in-memory + a JSON write-through; persistence across restarts of the dev server is enough.
- Device-to-device discovery without going through the registry. The hive already does that internally; the registry is the *dataspace's* view, not the devices' view.
- Cross-LAN discovery. mDNS + IP, both LAN-scoped.
- Public-internet exposure. Same `lan-only` tag the spec already carries.
- Strong auth for the registry. The `R24/R32` shared-secret model is layered on later; v1 trusts anything on the LAN, matching the rest of the prototype (`R30` — devices define their own security).

---

## High level

```
                       ┌────────────────────────────────────┐
                       │  mock-join-server  (port 3001)     │
                       │                                    │
   ┌────────┐  POST    │   POST /api/v1/registry/announce   │   GET    ┌──────────────┐
   │ device │ ────────►│   POST /api/v1/registry/heartbeat  │ ◄──────  │ d3-spatial   │
   │ (esp32)│          │   POST /api/v1/registry/depart     │          │ (manifest    │
   └────────┘          │   GET  /api/v1/registry/devices    │          │  loader +    │
       ▲    DHCP /     │   GET  /api/v1/registry/devices/:id│          │  vite proxy) │
       │    mDNS       │                                    │          └──────────────┘
       │               │   In-memory + ~/.magnet-registry.  │
       │               │      json write-through for        │
       │               │      reboot persistence            │
       │               └────────────────────────────────────┘
       │
   reset / wifi-connect / craw_registry_announce()
```

Devices announce themselves on boot (after WiFi association). The server holds a registry. The dataspace consults the registry at manifest-load time to resolve `udm_uuid` → host. The Vite proxy reads the same registry at startup to populate its `target` fields.

## Wire surface

### `POST /api/v1/registry/announce`

Body: a slim UDM/USM extract for one device. Same field names as the manifest's `udm_devices` + `usm_services` so the server can persist exactly what the manifest wants to read.

```json
{
  "udm_key":    "magnet-imu-c070",
  "udm_uuid":   "urn:device:imu:m5capsule-c070",
  "udm_model_name": "M5Capsule (BMI270)",
  "udm_vendor": "M5Stack / Bosch Sensortec",
  "udm_type":   "motion-sensor",
  "udm_class":  ["motion-sensor", "imu", "vehicle"],
  "udm_capabilities": ["orientation-euler", "angular-velocity", "linear-acceleration", "madgwick-ahrs"],
  "udm_tags":   ["vehicle", "real-hardware", "bmi270"],

  "udm_spatial_anchor_x": { "x": 0.0, "y": 1.2, "z": -0.6 },

  "endpoints": {
    "host":   "10.0.0.119",
    "mdns":   "magnet-scribe-c070.local",
    "port":   80,
    "scheme": "http"
  },

  "usm_services": [
    {
      "usm_key":   "imu-snapshot",
      "usm_type":  "sensor",
      "usm_service_endpoint": { "GET": { "url": "/api/v1/sensor/imu",
                                         "content-type": "application/json" } },
      "characteristics": [ /* same shape as manifest */ ]
    }
  ],

  "heartbeat_interval_s": 60,
  "fw_version": "vitals-e4th-0.3.1"
}
```

Server responds:

```json
{
  "registered": true,
  "registry_id": "magnet-imu-c070",
  "server_time_ms": 1748137600000,
  "next_heartbeat_in_s": 60
}
```

The `udm_uuid` is the stable identifier; `udm_key` is the human-readable handle. If a device announces with an existing `udm_uuid` the server **replaces** the entry (handles DHCP renewals and firmware updates without orphan rows).

### `POST /api/v1/registry/heartbeat`

```json
{ "udm_uuid": "urn:device:imu:m5capsule-c070" }
```

Bumps `last_seen_ms`. Cheap. Devices send this every `heartbeat_interval_s`. Server treats a device as **online** while `(now - last_seen_ms) < 3 * heartbeat_interval_s`, then **stale**, then **expired** at `5 * heartbeat_interval_s` and drops from `GET /devices`.

### `POST /api/v1/registry/depart`

```json
{ "udm_uuid": "urn:device:imu:m5capsule-c070", "reason": "shutdown" }
```

Optional, best-effort. Devices call this from a shutdown hook if possible (the Capsule's `power-off` Forth word, the camera's reboot path). If a device crashes hard the heartbeat-expiry path takes over.

### `GET /api/v1/registry/devices`

```json
{
  "devices": [
    { /* full announce body */ "online": true, "last_seen_ms": ..., "first_seen_ms": ... },
    ...
  ],
  "generated_ms": 1748137600000
}
```

Optional query params:
- `?tag=health-data` — filter by tag
- `?capability=heart-rate` — filter by capability
- `?type=motion-sensor` — filter by UDM type

### `GET /api/v1/registry/devices/:uuid_or_key`

Same shape, one device or 404.

## Server implementation sketch

In `server/mock-join-server.ts`, add a `Map<uuid, DeviceRecord>` plus a write-through to `~/.magnet-registry.json` (or `./registry.json` next to the server bundle). Persistence is for "you restarted Vite, don't lose the registry" — not durability.

```ts
type DeviceRecord = AnnounceBody & {
  online: boolean;
  first_seen_ms: number;
  last_seen_ms:  number;
};

const registry = new Map<string, DeviceRecord>();
const REG_PATH = path.resolve(process.env.MAGNET_REGISTRY_PATH ?? './registry.json');

function loadRegistry()    { /* read REG_PATH, parse, hydrate Map */ }
function persistRegistry() { /* JSON.stringify; debounced 1s write */ }
function ageOut()          { /* sweep entries past 5× heartbeat_interval_s */ }

setInterval(ageOut, 5_000);
```

Concurrency: only one writer (the express handlers). No locks needed.

## Device-side: `craw_registry` component

A small new ESP-IDF component, sibling to `craw_imu` / `craw_redis` / `craw_hive`. About ~200 LOC, mirrors `craw_mqtt`'s shape (single config + start/stop/announce/heartbeat lifecycle).

### Public surface (`craw_registry.h`)

```c
typedef struct {
    const char *server_url;      // e.g. "http://magnet-join.local:3001"
                                 // — discovered via mDNS lookup of
                                 //   _magnet-join._tcp at startup;
                                 //   falls back to a build-time default
                                 //   baked into firmware
    int         heartbeat_s;     // default 60
    const char *udm_json_path;   // NVS key holding the device's UDM
                                 // metadata blob; written once by the
                                 // BLE provisioner, persistent
} craw_registry_config_t;

esp_err_t craw_registry_init(const craw_registry_config_t *cfg);
esp_err_t craw_registry_announce(void);  /* one-shot POST + persist */
esp_err_t craw_registry_start(void);     /* periodic heartbeat task */
esp_err_t craw_registry_stop(void);
esp_err_t craw_registry_depart(const char *reason);
```

### Boot flow (UC4 IMU example)

```
1. Capsule boots, runs the existing boot ritual.
2. craw_wifi_init → on EVENT_CONNECTED:
     - mDNS resolve _magnet-join._tcp → server URL (mock-join-server
       advertises itself with this service type).
     - craw_registry_init({ server_url, heartbeat_s: 60,
                            udm_json_path: "udm_blob" })
     - craw_registry_announce()
     - craw_registry_start()
3. The IMU sample task + httpd come up as today.
4. Every 60 s, craw_registry POSTs heartbeat.
5. On `power-off` Forth word: craw_registry_depart("shutdown").
```

### Where does the UDM blob come from?

Two sources, in priority order:

1. **BLE provisioner** — the user enters the dataspace owner email + flips device tags (`vehicle`, `personal`) in the existing `MagNET` BLE provisioning flow. The provisioner builds the full UDM JSON and writes it to NVS under `udm_blob`. This is the production path.

2. **Compile-time default** — each firmware project ships a `firmware_udm.json` next to `platformio.ini`. CMake bakes the bytes into `_binary_firmware_udm_json_start` (the same EMBED_FILES path the BMI270 driver uses for its config blob). Used during dev when the device hasn't been provisioned yet; reads its `udm_uuid` and the rest from the blob.

The BLE provisioner already writes to NVS for WiFi creds + hive role + role-bundle — extending it to also write the UDM blob is the smallest delta.

### MAC-suffix substitution

The compile-time UDM contains placeholder `udm_key: "magnet-imu-XXXX"` and `udm_uuid: "urn:device:imu:m5capsule-XXXX"`. On first boot, `craw_registry_init` reads the WiFi MAC, derives the 4-hex-char suffix (matching the hostname format `magnet-scribe-c070`), and substitutes before announce. Persists the resolved JSON to NVS on first run so subsequent boots skip the substitution.

## Dataspace integration

### Manifest changes

`udm_devices` becomes a list of *requirements*, not hard identifiers:

```json
"udm_devices": [
  {
    "udm_uuid_ref":      "urn:device:imu:m5capsule-*",   // wildcard ok
    "udm_class_required": ["motion-sensor", "imu"],
    "udm_tags_required":  ["vehicle"]
  }
]
```

The manifest loader resolves these references against `GET /api/v1/registry/devices?type=motion-sensor&tag=vehicle` and picks the best match (most-specific tag overlap, or first online). The chosen device's full UDM is then folded into the manifest's `udm_devices` for the cells to consume — same shape as today, just runtime-bound.

Marks continue to cite `deviceRef: "<udm_key>"`; the loader rewrites this to the resolved id.

### Vite proxy changes

Replace the env-var-driven `target` fields with a registry consultation at startup:

```ts
// vite.config.ts (sketch)
async function resolveDeviceHosts(): Promise<Record<string, string>> {
  const resp = await fetch('http://localhost:3001/api/v1/registry/devices');
  const json = await resp.json();
  const map: Record<string, string> = {};
  for (const d of json.devices) {
    if (!d.online) continue;
    const url = `${d.endpoints.scheme}://${d.endpoints.host}:${d.endpoints.port}`;
    /* Build a route for each USM service this device exposes. */
    for (const s of d.usm_services) {
      const path = s.usm_service_endpoint?.GET?.url;
      if (path) map[path] = url;   // path → target host
    }
  }
  return map;
}

const deviceMap = await resolveDeviceHosts();
const proxy: Record<string, ProxyOptions> = {};
for (const [path, target] of Object.entries(deviceMap)) {
  proxy[path] = { target, changeOrigin: true, agent: makeAgent(), configure: stripHeaders };
}
proxy['/api/v1'] = { target: 'http://localhost:3001', changeOrigin: true };  // fallback
export default defineConfig({ server: { proxy } });
```

Implementation notes:
- Vite reloads on `vite.config.ts` change, so the registry is read on each dev-server boot. For DHCP renewals *during* a dev session, see §6 (hot-update).
- The proxy entries are still LITERAL paths (`/api/v1/sensor/imu` → IMU device); the existing single-socket `Agent` + header-strip patterns continue to apply.
- **Env vars stay valid** as a fallback layer: `IMU_HOST`, `CAMERA_HOST` etc. are applied AFTER the registry; an env var pinned by the user wins. This is the "I just plugged in a brand-new device that hasn't provisioned yet" escape hatch.

### Cells / manifest loader

No changes for v1. The loader already takes a UDM-shaped device entry and resolves URLs through it. Once the loader rewrites manifests against the registry, cells see the same shape they see today.

## Update propagation (DHCP renewal mid-session)

For a dev session that runs longer than a DHCP lease:

- The registry's `heartbeat` path catches the IP change — on the device's next heartbeat after the renew, the announce body's `endpoints.host` is the new IP. The server replaces the existing record (same `udm_uuid`).
- The Vite proxy reads device hosts at **startup**. To pick up an IP change mid-session we'd need either (a) a manual `npm run dev` restart, (b) a Vite plugin that subscribes to `/api/v1/registry/events` (SSE) and updates `http-proxy` targets in-place, or (c) the dataspace itself short-circuits through the join server's pass-through (`/api/v1/<device-id>/<path>` proxied server-side).

For v1, accept (a). For v2 ship (b) as a small Vite plugin (~50 LOC).

## Migration plan

Sized to be implementable in ~3–4 focused sessions.

### Phase 1 — Server registry endpoint (mock-join-server)

- Add the 4 routes + the `Map<uuid, DeviceRecord>` + JSON write-through.
- Add `/api/v1/registry/events` SSE for v2.
- Tests: 6–8 new tests in `server/mock-join-server.test.ts` (announce, heartbeat refresh, expiry sweep, tag filter, depart, persistence round-trip).

### Phase 2 — `craw_registry` component + one device

- New ESP-IDF component as described above.
- Wire it into the M5Capsule (it already has httpd; the announce URL is also via httpd which makes it easy to test).
- Forth words `registry-on` / `registry-off` / `registry-status` mirroring the imu/redis surface.

### Phase 3 — Vite proxy registry consultation

- Replace the explicit `*_HOST` env var blocks with `resolveDeviceHosts()` at startup.
- Keep env vars as the FALLBACK so an unprovisioned device is still reachable.
- README update under each firmware project documenting the new flow.

### Phase 4 — Manifest loader resolves UDM refs

- `loader.ts` learns `udm_devices` entries with `udm_uuid_ref` (wildcard or exact).
- At load time, query `/api/v1/registry/devices` and rewrite the manifest in place before passing to the cell builders.
- Update each UC manifest (UC1/2/4 — UC3 has no real devices) to use refs.

### Phase 5 — Remaining devices

- Add `craw_registry` to the AHT20 env sensor, the ESP32-CAM, the boombox, the lighting strip, the vitals C6.
- One device per session; each one ~30 minutes of work once the component exists.

### Phase 6 — Vite SSE plugin for live IP updates

- ~50 LOC Vite plugin subscribing to `/api/v1/registry/events`, mutates `httpProxy` targets in place on device updates.

## Open questions

1. **Where does the dataspace get the registry server URL?** Right now the dataspace assumes `localhost:3001` via Vite proxy. Across cloudflared tunnels this still works (relative `/api/v1/...`). For a future hosted dataspace, we'd advertise the registry URL in the dataspace manifest (`registry_url` top-level field).
2. **Does the registry need authentication for v1?** Probably not — `lan-only`, dev-time. Add it when we add R23/R24/R32 PKI.
3. **mDNS service type for the join server.** Proposed: `_magnet-join._tcp`. Confirm with the wider spec — `PROPOSAL.md` doesn't pick one.
4. **Manifest-side schema for ref devices.** Should `udm_uuid_ref` use exact / regex / glob? Glob is the lowest-cognitive-overhead default.
5. **What about devices that aren't in the registry but the user wants to test?** Env-var fallback covers it today; long-term we could add a "manual entry" UI in the join panel that POSTs to `/api/v1/registry/announce` on the user's behalf.
6. **Per-device firmware UDM JSON file** — where does it live? Proposal: each firmware project has `udm/firmware_udm.json` next to `platformio.ini`. Hive role-bundle install can also write to NVS to mutate it post-flash.

---

**TL;DR:** add 4 HTTP routes to `mock-join-server`, ship a ~200-LOC `craw_registry` ESP-IDF component, and replace `*_HOST` env vars in `vite.config.ts` with a registry consultation at startup. Devices announce themselves, the dataspace asks the server, the proxy routes accordingly. Env-var fallback stays valid so unprovisioned devices still work.

---

## Alternative: dev-side mDNS scan, no firmware changes (LANDED 2026-05-22)

Implementation lives in `prototype/d3-spatial/tools/discover-magnet-devices.mjs` (zero-deps Node script) + the `discoverHostsSync()` block in `prototype/d3-spatial/vite.config.ts`. Verified working against the live LAN — the dev server logs the discovery on startup and the per-device proxy entries pick the freshest host.

Every existing MagNET firmware *already* publishes the data the registry would need — they register `_magnet-node._tcp` (or `_magnet-imu._tcp` for the Capsule's IMU mode, or `_http._tcp` for older builds), set TXT records like `role=spy / role=boombox / role=lighting` with a `caps=` list, and advertise an mDNS hostname (`magnet-cam-8610.local`, `magnet-boombox-4924.local`, …). On macOS, all of that is queryable from `dns-sd`, the OS-built-in Bonjour CLI.

That means the entire registry can be **derived on the dev machine** by browsing mDNS — no `craw_registry` component, no `/api/v1/registry/announce` routes, no firmware churn. Tag the repo first, ship the dev-side scan now, do the device-side proposal above as a follow-up.

### Verified live LAN scan

`tools/discover-magnet-devices.mjs` (~150 LOC, zero npm deps, pure ES modules + `child_process`) wraps `dns-sd -Z` and `dns-sd -G v4`, parses the zonefile-format output, and emits JSON. Running it just now against the test LAN produced:

```
$ node tools/discover-magnet-devices.mjs
Found 3 MagNET device(s) in 3s:

  magnet-cam-8610.local              → 10.0.0.188     :80
    instance:  MagNET Hive Camera
    services:  _magnet-node._tcp, _http._tcp
    txt:       ver=1 · caps=camera,jpeg · role=spy

  magnet-boombox-4924.local          → 10.0.0.121
    instance:  MagNET Hive Boombox
    services:  _magnet-node._tcp
    txt:       ver=1 · caps=audio,speaker,tone · role=boombox

  magnet-lighting-b7c0.local         → 10.0.0.144     :80
    instance:  MagNET Lighting
    services:  _http._tcp
    txt:       model=WS2813 · role=lighting
```

So the host → IP map for the Vite proxy is right there, including a TXT-derived `role` we can use to disambiguate which device fills which env-var slot (camera, boombox, lighting today; vitals + atom-echo + scribe whenever they're powered on).

### Vite integration (as shipped)

`vite.config.ts` calls `discoverHostsSync()` once at startup; each per-device proxy entry uses `pickHost(envKey, fallback)` to resolve its target. **Precedence flipped from the original draft — discovered wins over env var**, on the principle that a freshly-resolved mDNS IP is closer to ground truth than a possibly-stale env var. Env vars still serve as the manual override when discovery returns nothing (off-LAN device, unprovisioned hardware, dns-sd not available).

```ts
import { execSync } from 'node:child_process';

/* Spawn the discovery tool once at vite-config evaluation time
 * (synchronous because defineConfig() runs synchronously). Cap the
 * spawn at 5 s so a stalled mDNS responder doesn't block the dev
 * server forever; on timeout / parse error, fall through to env
 * vars + hardcoded .local hostnames as before. */
function discoverHostsSync(): Record<string, string> {
  if (process.platform !== 'darwin') return {};   // Linux path TBD
  try {
    const out = execSync(
      'node ./tools/discover-magnet-devices.mjs --json --timeout 3',
      { timeout: 5000, encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] },
    );
    const devs = JSON.parse(out) as Array<{
      hostname: string; host: string; ip: string;
      txt: Record<string, string>;
    }>;
    const map: Record<string, string> = {};
    /* Map TXT role + hostname prefix to existing env-var slots.
     * Convention: anything we'd previously have set with an env
     * var lands in the same key here, so the precedence cascade
     * (env > discovered > hardcoded .local) is one line per
     * device below. */
    for (const d of devs) {
      const url = `http://${d.ip}`;
      const role = (d.txt.role ?? '').toLowerCase();
      const cap  = (d.txt.caps ?? '').toLowerCase();
      if (role === 'spy'      || cap.includes('camera'))    map.CAMERA_HOST   = url;
      if (role === 'boombox'  || cap.includes('speaker'))   map.BOOMBOX_HOST  = url;
      if (role === 'lighting' || cap.includes('neopixel'))  map.LIGHTING_HOST = url;
      if (role === 'vitals'   || cap.includes('heart'))     map.VITALS_HOST   = url;
      if (role === 'env-sensor' || cap.includes('aht20'))   map.ENV_HOST      = url;
      /* The Capsule advertises _magnet-imu._tcp ONLY when `imu-on`
       * has been run — so a discovered magnet-scribe-* host is
       * only the IMU when that service shows up. The script
       * preserves the services[] array; gate on it. */
      if (cap.includes('imu')) map.IMU_HOST = url;
    }
    return map;
  } catch (e) {
    console.warn('[vite] mDNS device discovery skipped:', (e as Error).message);
    return {};
  }
}

const discovered = discoverHostsSync();

/* Existing pattern, just with the discovered value as middle
 * precedence between env var and hardcoded fallback. */
function pickHost(envKey: string, fallback: string): string {
  return normalizeHost(process.env[envKey] ?? discovered[envKey], fallback);
}

// then in the proxy block:
'/camera':           { target: pickHost('CAMERA_HOST',   'http://magnet-cam-80e4.local'),    /* … */ },
'/api/v1/sensor':    { target: pickHost('ENV_HOST',      'http://magnet-atom-echo.local'),   /* … */ },
'/api/v1/vitals':    { target: pickHost('VITALS_HOST',   'http://magnet-vitals.local'),      /* … */ },
'/api/v1/actuator/neopixel':
                     { target: pickHost('LIGHTING_HOST', 'http://magnet-lighting.local'),    /* … */ },
'/api/v1/actuator/speaker':
                     { target: pickHost('BOOMBOX_HOST',  'http://magnet-boombox.local'),     /* … */ },
'/api/v1/sensor/imu':{ target: pickHost('IMU_HOST',      'http://magnet-scribe.local'),      /* … */ },
```

### Trade-offs vs the firmware-side approach

| | dev-side mDNS scan | firmware `craw_registry` |
|---|---|---|
| Firmware churn | none | ~200 LOC × 6 devices |
| Setup latency | one-time 3 s at `npm run dev` | none |
| DHCP renewals mid-session | manual restart | manual restart (v1), SSE (v2) |
| Cross-platform | macOS only (dns-sd) — Linux trivial via avahi-browse | works everywhere |
| Works behind tunnels | only if mDNS responder runs on the dev box | yes (HTTP roundtrip via join server) |
| Authoritative registry | none — derive each time | central registry, survives restarts |
| Manifest UDM-ref resolution | needs separate plumbing if we want it | natural — registry already speaks UDM |
| Needs `craw_registry` ESP-IDF component | no | yes |

### Recommendation

Ship the dev-side mDNS scan FIRST as the "today" path — it's verified working against the actual LAN, requires no firmware re-flash, and converts the dev workflow from "edit env var, restart Vite" to "just run Vite, it finds whatever's on." Then later, when firmware churn is acceptable again, layer on `craw_registry` for the production-style path (works from any client without dns-sd, survives DHCP mid-session via SSE, plays nice with manifest UDM refs).

### Known limitations of the dev-side path

- **macOS only.** `dns-sd` is Apple's tool. Linux uses `avahi-browse -arpt _magnet-node._tcp`; a small `if (platform() === 'linux')` fork in the script would close the gap. Windows would need `dns-sd.exe` from Bonjour SDK.
- **macOS Local Network permission must be granted** to whichever terminal or `node` process runs the discovery — see `docs/macOS-LAN-networking.md`. Without it, dns-sd returns no devices and the scan looks "broken" silently.
- **iCloud Private Relay** routes some LAN traffic through Apple proxies; disable it during dev sessions (`System Settings → Apple Account → iCloud → Private Relay`).
- **Private Wi-Fi MAC** changes the device's perceived MAC each time the Mac joins a network, which the firmware doesn't actually see — but it does mean ARP-table lookups are not stable across reboots if you wanted to match against MAC OUIs as a sanity check.
- **3-second scan window** at startup is the default. Slow-to-respond devices may need `--timeout 5` or higher; pass that through in the `execSync` call. Cheap to bump.
- **Capsule IMU disambiguation.** `magnet-scribe-c070.local` could be a hive scribe with no IMU or a scribe with `imu-on` running. The proposed gate uses `txt.caps.includes('imu')` because the Capsule firmware adds `_magnet-imu._tcp` and updates the `caps` TXT only when `imu-on` is active.

### What devices report today (verified)

Per the live scan + a code-grep of every firmware project's `mdns_service_add` + `mdns_hostname_set` calls:

| Firmware                          | Hostname pattern              | mDNS service        | TXT role        | TXT caps                       |
|-----------------------------------|-------------------------------|---------------------|-----------------|--------------------------------|
| `M5_Hive_Camera`                  | `magnet-cam-<MAC4>`           | `_magnet-node._tcp` + `_http._tcp` | `spy`           | `camera,jpeg`                  |
| `MagNET_ReSpeaker_Boombox`        | `magnet-boombox-<MAC4>`       | `_magnet-node._tcp` | `boombox`       | `audio,speaker,tone`           |
| `XIAO_ESP32C3_IOT_LIGHTING`       | `magnet-lighting-<MAC4>`      | `_http._tcp`        | `lighting`      | `model=WS2813`                 |
| `M5Capsule_Hive_Scribe_Redis`     | `magnet-scribe-<MAC4>`        | `_magnet-node._tcp` (always) + `_magnet-imu._tcp` (when `imu-on`) | `scribe` | `scribe,kv-store` (+ `imu` when active) |
| `MagNET_Vitals_E4TH`              | `magnet-vitals`               | `_http._tcp`        | (none)          | (none)                         |
| `M5Atom_Echo_Hex_Hive_ATH20`      | `magnet-atom-echo`            | (TBD — confirm)     | (TBD)           | (TBD)                          |

The two TBD lines deserve a one-line read of those firmware projects' `mdns_service_add` calls; the pattern is enough to act on now and refine later. If we want consistent role + caps tags across every device, that's a one-line change per firmware (a minor follow-up, but again, *not blocking* the dev-side discovery from working today).

