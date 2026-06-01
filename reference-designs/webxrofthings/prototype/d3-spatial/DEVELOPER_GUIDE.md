# d3-spatial — Developer guide

How to build your own dataspace on top of d3-spatial. This guide is for **dataspace authors** — people who want to publish a spatial visualisation that loads in a WebXR headset, backed by their own devices, real or simulated, without writing renderer code.

If you want to extend d3-spatial itself (add a new mark type, change rendering primitives, contribute back), see [`CONTRIBUTING.md`](./CONTRIBUTING.md). If you want the full method/class reference, see [`API.md`](./API.md). If you want to know what's still pending, see [`STATUS.md`](./STATUS.md).

---

## Mental model

A **dataspace** is a JSON manifest that describes a small XR scene of data marks anchored in front of a user wearing a Quest 3 or Snap Spectacles. The renderer loads the manifest, queries each mark's data source, instantiates a viz primitive, and arranges everything in a grid in front of the user.

You author *one JSON file*. The runtime does the rest.

```
                    ┌─────────────────────────────────────────┐
                    │ examples/my-room.json (a Manifest)      │
                    │   { marks: [                             │
   you write this → │     { id, type, data, config, … },     │
                    │     { id, type, data, config, … }      │
                    │   ] }                                    │
                    └────────────────────────┬────────────────┘
                                             │
                                             ▼ loadManifest()
                                ┌──────────────────────────┐
                                │ manifest/loader.ts       │
                                │  - pre-fetches URLs      │
                                │  - dispatches to builders│
                                │  - schedules refresh     │
                                │  - tracks health         │
                                └────────────┬─────────────┘
                                             │
                                             ▼ renderManifestToScene()
                                ┌──────────────────────────┐
                                │ manifest/renderManifest  │
                                │  - grid layout           │
                                │  - cell title + subtitle │
                                │  - Interact registration │
                                │  - DEMO HUD / offline HUD│
                                └────────────┬─────────────┘
                                             │
                                             ▼ attached to vizAnchor
                                ┌──────────────────────────┐
                                │ THREE.js scene, in front │
                                │ of the user at eye level │
                                └──────────────────────────┘
```

The four canonical manifests under `examples/` (`uc1-vitals.json`, `uc2-room.json`, `uc3-poster.json`, `uc4-airplane.json`) are working references — read them alongside this guide.

---

## Setup

Prerequisites: Node 20+, npm, a Quest 3 or Spectacles for on-device testing (desktop preview also works).

```bash
git clone <repo>
cd reference-designs/webxrofthings/prototype/d3-spatial
npm install
```

Two processes run in parallel during dev:

```bash
# Terminal 1 — the join-code server (handles DEMO01-04 + rotating codes)
npm run server

# Terminal 2 — the Vite dev server (the dataspace renderer)
npm run dev
```

If port 3001 is taken by another project, run both on a free port:
```bash
JOIN_SERVER_PORT=3101 npm run server
JOIN_SERVER_PORT=3101 npm run dev
```

Open `http://localhost:5173/?manifest=/examples/uc1-vitals.json` for direct manifest load (skips the join-code flow). Or open `http://localhost:5173/` and type `DEMO01` into the keypad to enter UC1.

For headset access, expose Vite over HTTPS via `cloudflared`:
```bash
cloudflared tunnel --url http://localhost:5173
```
Open the resulting `*.trycloudflare.com` URL on the headset's browser. See `examples/uc2-room.md` for the macOS Local-Network-permission gotchas and `docs/macOS-LAN-networking.md` for the broader debug.

---

## Your first manifest

A minimal one-mark dataspace:

```jsonc
{
  "version": "1",
  "name": "hello-spatial",
  "displayTitle": "Hello, spatial D3",
  "scaleTag": "personal",
  "owner": "you@example.com",
  "acousticEnvironment": "indoor",

  "marks": [
    {
      "id": "hello",
      "type": "line",
      "title": "A line",
      "subtitle": "made of two points",
      "data": {
        "source": "inline",
        "series": { "samples": [
          { "t": 1000, "v": 10 },
          { "t": 2000, "v": 90 }
        ] }
      },
      "config": { "vMin": 0, "vMax": 100, "color": 16753280 },
      "hoverable": true
    }
  ],

  "hud": {
    "position": "bottom",
    "items": [
      { "id": "recenter", "label": "Recenter", "icon": "+", "action": "recenter" },
      { "id": "leave",    "label": "Leave",    "icon": "x", "action": "leave-dataspace" }
    ]
  }
}
```

Save it as `public/manifests/hello.json`, then visit:

```
http://localhost:5173/?manifest=/manifests/hello.json
```

You'll see the line mark with a "Hello, spatial D3" title above it, plus a bottom HUD with Recenter and Leave.

### The required top-level fields

| Field | What it does |
|---|---|
| `version` | Manifest spec version. `"1"` for now. |
| `name` | URL-safe id of the dataspace. Used in logs + telemetry. |
| `displayTitle` | What the user sees as the "dataspace title" floating above the cell grid. |
| `scaleTag` | One of `personal` / `room` / `hall` / `vehicle`. Drives the dataspace-chip glyph + color. |
| `owner` | Email or URN of who created this. Surfaced in the privacy banner. |
| `acousticEnvironment` | `indoor` / `outdoor` / `vehicle`. Drives the ambient audio bed. |
| `marks` | Array of `MarkSpec` — the things you actually see. |
| `hud` | Optional. `{ position, items: [{ id, label, icon, action }] }`. Renders the per-dataspace HUD strip. |

### Optional UDM/USM blocks

For dataspaces that connect to real IoT devices, include `udm_devices` (device descriptions) and `usm_services` (service descriptions). They're metadata-only today (the renderer doesn't act on them yet) but power future features (device pin glyphs at spatial anchors, service-list side panels). See `uc2-room.json` and `uc4-airplane.json` for fully-worked examples, and `specs/UDM-MagNET-v1.md` for the schema.

---

## Mark types

Each `MarkSpec.type` dispatches to a builder. The full set ships in `src/manifest/builders.ts`:

| Type | What it does | Common config |
|---|---|---|
| `line` | Polyline ribbon over a time series. | `vMin`, `vMax`, `color`, `width`, `height` |
| `arc` | Radial gauge reading the latest sample. | `min`, `max`, `radius`, `tubeRadius` |
| `bar` | Vertical bar chart. | `vMin`, `vMax`, `color`, `barWidth` |
| `streamgraph` | Scrolling animated waveform (heart/breath/total style). | `categories`, `windowSize`, `scrollSpeed` |
| `tree` | Radial / wall node-link diagram. | `form`, `radius`, `palette` |
| `treemap` | Extruded city-block layout. | `width`, `height`, `palette` |
| `sunburst` | Stacked discs. | `radius`, `depthSpacing` |
| `pack` | Nested 3D spheres. | `size`, `palette` |
| `tidy-tree` | Reingold-Tilford on a cylindrical surface. | `radius`, `height` |
| `tangled-tree` | Spine + tangle arcs (control + sync). | `tangles`, `arcDepth` |
| `parallel` | Vertical axis rods with line crossings. | `dimensions`, `palette` |
| `edge-bundle` | Radial dendrogram with bundled edges. | `beta`, `maxEdges` |
| `force` | d3-force-3d graph, grabbable nodes. | `size`, `ticks`, `palette` |
| `force-tree-3d` | Force-directed *tree* (cluster blob). UC3 art. | `clusters`, `leavesPerCluster`, `size`, `radius` |
| `sankey` | 3D flow tubes with proportional cross-section. | (data-driven) |
| `ridgeline` | Density ribbons per row, animated. | `palette`, `rowHeight` |
| `video` | HLS / MJPEG / polled JPEG frames panel. | `type`, `url`, `width`, `aspectRatio`, `fallbackImageUrl` |
| `imu` | Live airplane attitude + globe + KT/HDG/FT labels. | `size`, `color`, `smooth` |
| `spatial-audio` | Wireframe boombox playing procedural music. | `size`, `refDistance`, `gain`, `autoplay` |
| `splat-gallery` | Gaussian-splat photo carousel. | `photos`, `autoAdvanceMs`, `splatY`, `sceneRotation` |
| `actuator-panel` | UC2-style control panel (light / strip / thermostat / speaker). | (inline data) |
| `voronoi-stippling` | Bostock-style stipple over an image, optionally prism. | `imageUrl`, `samples`, `width`, `height`, `prism`, `attribution` |
| `moon-phases-arc` | 29 moons around a 300° arc. | `count`, `arcDegrees`, `radius`, `moonSize`, `height` |
| `owls-to-the-max` | Bostock owl grid on a ceiling tile. | `cols`, `rows`, `width`, `height`, `ceiling`, `floorY` |

The `config` block is type-specific; copy from `examples/` and adapt. Defaults are sensible for "drop it on a manifest and it works."

### Data sources (`MarkSpec.data`)

```jsonc
{
  // Inline JSON, baked into the manifest:
  "data": { "source": "inline", "series": { "samples": [...] } }
}
{
  // URL fetched at load + periodically:
  "data": { "source": "url",
            "url": "/api/v1/vitals/heart-rate/history",
            "shape": "series",
            "refreshInterval": 30 }
}
```

`shape` tells the loader what JSON contract to expect. Supported shapes today:

| Shape | Expected payload |
|---|---|
| `series` | `{ samples: [{ t: number, v: number }, …] }` — time series |
| `distributions` | `[[…], [...], […]]` — N parallel waveforms (streamgraph) |
| `hierarchy` | `{ name, children: [...] }` — d3-hierarchy compatible |
| `graph` | `{ nodes: [...], links: [...] }` — for force graphs |
| `video` | URL-only; the cell polls/streams the URL directly |
| `imu` | URL-only; `{ orientation, angular_velocity, acceleration, timestamp_us }` |

When a URL fetch fails repeatedly, the loader switches to a **fake-data generator** (see `src/manifest/fakeData.ts`) and the **DEMO MODE** badge + **offline-sensors** card appear in the HUD. Your charts keep moving; the user can tell from the chrome that the data isn't real. The fake values respect your `vMin`/`vMax` config so a narrow-range chart doesn't get scrawled across the scene.

### Polling cadence — slower than you think, staggered when sharing a chip

Pick `refreshInterval` to match how often the underlying signal *actually changes*, not how often you'd like the chart to move. Polling too fast doesn't make the data fresher — it just hammers the device's HTTP server. Rough guide for typical IoT-class sensors:

| Signal | Sensible cadence | Why |
|---|---|---|
| Heart rate, breathing | 1–3 s | Beat-to-beat variability is the signal itself |
| IMU / orientation | 100–500 ms (self-fetching) | Smoothness of motion matters; cells dead-reckon between polls |
| Camera frame | 1–2 s | Throughput-limited by chip + Vite proxy maxSockets:1 |
| Ambient temperature, humidity | **30–60 s** | Room temp changes by ~0.1°C per minute; faster is just waste |
| Pressure, AQI, light, pollen | 30–60 s | Same — environmental signals are slow |
| Actuator state (light on/off) | event-driven, not polled | Mark refreshes via direct response to the POST |

For the slow signals (env, baro, AQI), `5 s` is **12× faster than the signal changes**. On a small chip like the M5 Atom Echo, that rate can drive the esp_http_server's parser into edge cases (we've seen 100 % deterministic `431 Request Header Fields Too Large` on humidity at 5 s polls when temperature is also polling at 5 s on the same device — see [the 2026-05-25 debug session](#) and the firmware-side memo).

**Staggering — the `startDelayMs` field.** When two marks poll the same chip at the same cadence, they fire at the same instant by default (manifest pre-fetch happens at *t* = 0 for all marks in parallel; the scheduled `setInterval` then ticks them all together every cadence). To offset them, set `startDelayMs` on the second mark:

```jsonc
{ "id": "room-temp",
  "data": { "source": "url", "url": "/api/v1/sensor/temperature/history",
            "shape": "series", "refreshInterval": 30 } },
{ "id": "room-humidity",
  "data": { "source": "url", "url": "/api/v1/sensor/humidity/history",
            "shape": "series", "refreshInterval": 30,
            "startDelayMs": 15000 } }
```

Both poll at 30 s, but humidity is offset by 15 s — so the chip never sees both requests at the same instant. The pre-fetch on manifest load still fires for both at *t* = 0 (so you get data on first paint), but the **scheduled** refresh cadence is staggered for the rest of the session.

For three or more marks on the same device at the same cadence, spread them evenly: with three at 30 s, use `startDelayMs: 0`, `10000`, `20000` — one poll every 10 s instead of three at once every 30 s. The device's httpd is dramatically happier with one-at-a-time polls vs simultaneous bursts.

---

## Wiring real devices

Two paths, in priority order, decide the URL each `*_HOST`-style proxy entry resolves to:

1. **Environment variable** (operator override): `IMU_HOST=http://10.0.0.119 npm run dev` — always wins when set.
2. **mDNS auto-discovery**: on macOS, Vite startup runs `tools/discover-magnet-devices.mjs` which browses Bonjour for `magnet-*.local` hostnames advertising `_magnet-node._tcp` / `_magnet-imu._tcp` / `_http._tcp`. Each device's TXT `role=`/`caps=` maps onto the matching `*_HOST` slot.
3. **Hardcoded `.local` fallback** — each proxy entry has a default like `http://magnet-cam-8610.local` for the worst case.

Disable discovery if you want a clean baseline: `MAGNET_DISCOVERY=0 npm run dev`. The startup log shows each resolved target so you can triage:

```
[vite]   IMU_HOST       → http://10.0.0.119               (env)
[vite]   CAMERA_HOST    → http://10.0.0.188               (mdns)
[vite]   VITALS_HOST    → http://magnet-vitals.local      (fallback)
```

When the dataspace is loaded behind a `cloudflared` tunnel, the proxy rules strip the `Cf-*` / `X-Forwarded-*` headers and bump the ESP-IDF `httpd` request-header buffer accordingly — see `specs/device-self-registration.md` and `STATUS.md` for the gotchas.

The full firmware-side device-self-registration story (devices announcing themselves to a central registry) is proposed but not yet implemented — see `specs/device-self-registration.md`.

---

## Custom mark builders

When none of the built-in mark types fit, register your own. A builder is a `(spec: MarkSpec) => LoadedMark | null` that takes the parsed spec and returns a renderable cell.

```ts
// src/manifest/builders.ts (or your own file imported at app boot)
import * as THREE from 'three';
import { registerMarkBuilder } from './manifest/loader';
import type { MarkSpec, LoadedMark } from './manifest/loader';

registerMarkBuilder('my-cool-mark', (spec: MarkSpec): LoadedMark | null => {
  const cfg = (spec.config ?? {}) as { width?: number; color?: number };

  // 1. Build a THREE.Group containing whatever you render.
  const group = new THREE.Group();
  group.name = `my-cool-mark:${spec.id}`;
  const geo = new THREE.PlaneGeometry(cfg.width ?? 0.3, 0.2);
  const mat = new THREE.MeshBasicMaterial({ color: cfg.color ?? 0xffcc66 });
  const mesh = new THREE.Mesh(geo, mat);
  group.add(mesh);

  // 2. Optionally expose a viz object — anything you want main.ts /
  //    renderManifest to be able to talk to (refresh, setActive,
  //    getStatus, dispose, …).
  const viz = {
    group,
    refresh(updatedSpec: MarkSpec) {
      // Called every time data.refreshInterval fires for url-source
      // marks. spec.data has already been mutated to inline form
      // with the new payload — read it via updatedSpec.data.
    },
    dispose() {
      geo.dispose();
      mat.dispose();
    },
  };

  // 3. Return the LoadedMark contract.
  return {
    id:             spec.id,
    type:           spec.type,
    title:          spec.title,
    subtitle:       spec.subtitle,
    group:          viz.group,
    viz,
    drillable:      false,
    hoverable:      spec.hoverable ?? false,
    draggable:      false,
    defaultVisible: spec.defaultVisible ?? true,
    refresh:        viz.refresh,
  };
});
```

You'd also need to extend `MarkType` in `src/manifest/schema.ts` so TypeScript + the validator know about your new type.

**Common cell capabilities** (the manifest pipeline introspects these on your `viz` object):

| Method | When it's called |
|---|---|
| `viz.refresh(spec)` | After each URL refresh cycle (for `data.source === 'url'`). |
| `viz.tick(time)` | Per-frame, if you expose one — the manifest layer doesn't drive it automatically. Set up your own `onBeforeRender` on a renderable child for per-frame work. |
| `viz.setActive(boolean)` | Toggled by `show-only:<markId>` HUD actions (UC4 mode-switching). Pause polling, audio, animation when inactive. |
| `viz.getStatus()` | Return `{ state: 'live'\|'stale'\|'offline', lastSuccessAgoMs }`. Drives the per-dataspace offline HUD. |
| `viz.isAutonomous()` | Return `true` if the cell has fallen back to synthesised data after sustained failure. Flips the DEMO badge. |
| `viz.getInteractables()` | Return `[{ id, object, onSelect, onHoverIn, onHoverOut }]` for buttons/regions inside the cell that should be hover-pickable. |
| `viz.clickTarget` / `viz.onSelect` | Single-click handler on a top-level Object3D. Used by the boombox-cycle-music pattern. |
| `viz.nodeMesh` + `pinNode` / `unpinNode` / `getNodeWorldPosition` / `getNodeLabel` | The "force-graph" shape — Interact wires drag automatically. |
| `viz.dispose()` | Called on manifest replace + leave-dataspace. Free GL resources, clear timers, stop audio. |

If your cell needs to be **self-positioned** (e.g., it wraps the user, like the moon arc), add its `MarkType` string to the `SELF_POSITIONED` set in `src/manifest/renderManifest.ts`. It'll be attached directly to the manifest root instead of placed in the cell grid.

---

## Onboarding flow

End users enter a dataspace one of three ways:

| Method | Notes |
|---|---|
| **Join code keypad** | Type a 6-char code into the in-XR keypad. `DEMO01`–`04` always resolve to UC1–UC4; the rotating dev code on the server console works too. Codes are validated by `mock-join-server`. |
| **`?manifest=<url>` URL** | Bypasses the keypad, loads the named manifest directly. Useful for sharing a specific dataspace or for dev. The URL is relative to the dev server. |
| **`?scene=charts` etc.** | Bypasses both, loads the demo gallery (no manifest). Mostly for renderer development. |

To wire a new fixed join code (e.g., for your own use case): edit `DEFAULT_FIXED_CODES` in `server/mock-join-server.ts` and add a manifest filename. Restart `npm run server`.

For production deployments you'd swap `mock-join-server` for your own auth/manifest service speaking the same `POST /api/v1/join` + `GET /api/v1/manifest` contracts (see `JOINCODE_SPEC.md`).

---

## Patterns we've learned

Tradeoffs and conventions that emerged across UC1–UC4. Most live as in-code comments at the relevant call sites; this is a navigation index.

### Self-positioned marks (UC3 owls / moons / tree)

Cells that want to ignore the grid layout (because they wrap the user, hang above the head, etc.) live in the `SELF_POSITIONED` set in `renderManifest.ts`. The renderer adds `mark.group` directly to root and skips the per-cell title overlay. The cell owns its own positioning logic.

### Dead-reckoning between polls (UC4 IMU)

The IMU cell polls at 5 Hz nominal, but `cloudflared`-tunneled latency makes the effective rate 2–3 Hz. Between polls, the cell integrates the last `angular_velocity` reading at 60 fps to keep motion fluid. When a fresh poll arrives, the cell slerps back to the authoritative orientation. See `src/demo/liveImuCell.ts`'s `onBeforeRender`.

### Autonomous mode after sustained offline (UC4 IMU)

`liveImuCell` exposes `isAutonomous()`: true after 10 s of failed polls. The manifest layer treats that as "this cell is in DEMO mode" and lights the yellow pulsing badge + lists the cell on the offline-sensors card. Brief outages don't flicker the HUD (gate is 10 s, not the cell-level poller's faster offline threshold).

### Show-only HUD switching (UC4)

UC4 ships four mutually-exclusive cells (Flight Info / Music / Video / Photos) on one dataspace. The HUD action `show-only:<markId>` flips one cell's wrapper to `visible=true` and the others to `false`, then calls `viz.setActive(false)` on the others so they pause polling / audio / video / splat-cycling. See `applyShowOnly` in `src/main.ts`.

### Inline title vs cell-wrapper title (UC4 video panel)

`renderManifest` paints a per-cell title above each grid cell. Builders should NOT also paint their own title (that's what the UC4 video panel comment is about — passing `title` into `buildVideoPanel` produces a duplicated label). For self-positioned marks that span the full scene, render an internal title yourself.

### Fallback images for offline cells (UC2 camera)

The video cell accepts `fallbackImageUrl`. It preloads the image into a separate `Texture` at construction; on first live-fetch error, the cell swaps `panelMat.map` to the fallback. The user always sees *something* — the alternative is a black panel for the 30 s the Vite proxy spends timing out the dead camera.

### Triangular-prism arrangements (UC3 voronoi-stippling)

Three plane meshes at azimuths 120° apart around a central Y axis. Each face is tangent to an inscribed circle of radius `W × √3 / 6`. `Object3D.lookAt` makes the object's **+Z** point toward the target — opposite of what `Camera.lookAt` does — which trips up most first attempts (the splash logo's earlier breakage was this exact lookAt-direction footgun).

### Health monitor + DEMO mode infrastructure

Every URL-source mark is registered with a `HealthMonitor` (see `src/manifest/healthMonitor.ts`) at manifest-load time. On consecutive fetch failures the mark goes `live → stale → offline`. The manifest layer (`renderManifest.ts`) subscribes and surfaces:

- **Offline-sensors HUD** — red-outlined card listing each offline mark id.
- **DEMO MODE badge** — yellow pulsing card visible while any mark is offline.
- **Fake-data fallback** — `src/manifest/fakeData.ts` synthesises `series` and `distributions` payloads from the mark's id keywords (HR, BR, temp, lux, pollen, AQI, baro) AND respects `config.vMin`/`vMax` so values stay inside the chart range.

Self-fetching cells (UC4 IMU, UC2 video) bypass the loader's polling layer; they expose `getStatus()` + `isAutonomous()` so the manifest layer's 1 s pull-status loop can mirror their state into the same HUD.

### Cell dispose contract

Every `viz.dispose()` must:
- Stop polling / setInterval / setTimeout
- Stop audio (positional or otherwise)
- Detach `onBeforeRender` hooks (`mesh.onBeforeRender = () => {}`)
- Dispose THREE geometries + materials + textures
- Free troika `Text` widgets (`text.dispose()`)
- Remove DOM elements (hidden `<img>`/`<video>` for video panels)

Skipping any of these causes "leave + rejoin a dataspace stacks zombie work" — a recurring bug pattern in early UCs. The loader walks every mark on dispose; the cell just needs to honor the contract.

---

## Testing your dataspace

| Command | What it does |
|---|---|
| `npm run typecheck` | TypeScript-only, no emit. Catches schema / config type mismatches. |
| `npm test -- --run` | Vitest suite. Currently 244 tests across manifest schema, loader, builder dispatch, mock-join-server, cell behaviours. Add tests for your custom builders if they're non-trivial. |
| `npm run smoke` | Playwright-driven screenshot capture — captures `demo/shots/*.png` across the milestone scenes. Add your manifest to the smoke list if it should be tracked. |
| `npm run smoke:baseline` | Reset the baseline screenshots. Run when intentional visual changes land. |
| `npm run smoke:diff` | Pixel-diff the current run against baseline. CI guard against unintentional drift. |

Manual on-device walkthrough: see `test-plan.md` for the per-UC test scripts (UC1 ~15 min, UC2 actuator-curl loop, UC3 visual checks, UC4 mode switching). Each smoke-test step has a one-line "what to look for."

---

## Deploying to a headset

1. Bring up the dev servers (`npm run server` + `npm run dev`).
2. Expose Vite over HTTPS (WebXR refuses to start over plain HTTP):
   ```bash
   cloudflared tunnel --url http://localhost:5173
   ```
3. Open the tunnel URL on the headset's browser.
4. Either type your join code, or use `?manifest=/manifests/yours.json` directly.
5. Tap "Start AR." Audio kicks in on the first controller-trigger / pinch gesture (autoplay policy).

For production-ish deployments without a tunnel, see `examples/uc2-room.md` for the `mkcert`-based local HTTPS pattern. For Quest specifically, see `XR_UX_BEST_PRACTICES.md`.

---

## Where to go next

- **Full API reference** — [`API.md`](./API.md). Class-by-class, prop-by-prop. Generated from the actual source, not aspirational.
- **Per-UC docs** — `examples/uc1-vitals.md` (TBD), `examples/uc2-room.md`, `examples/uc3-poster.md`, `examples/uc4-airplane.md`. Each one walks through a real working dataspace including device wiring + smoke test.
- **Spec proposals** — `../*.md` at the repo root (`PROPOSAL.md`, `XR_UX-proposal1.md`, `USECASE_SPECS.md`). The "why this exists" backdrop.
- **What's deferred** — [`STATUS.md`](./STATUS.md) under "Deferred capabilities" lists everything the spec set proposes that we haven't built yet.
- **Renderer internals** — [`CONTRIBUTING.md`](./CONTRIBUTING.md) for adding a new mark type or UI component to d3-spatial itself.
- **Design history** — `DESIGN_NOTES.md` documents key tradeoff decisions (toolbar occlusion, multi-dataspace key management).

A good progression for someone new: read this guide, skim `examples/uc1-vitals.json` and `uc2-room.json` to see the patterns in real manifests, then copy one and modify. The manifest schema is forgiving enough that small experiments are cheap.
