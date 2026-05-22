# d3-spatial

A spatial-D3 prototype for the WebXR-of-Things proposal: a manifest-driven dataspace renderer that loads in a WebXR headset (Quest 3, Snap Spectacles), instantiates 24+ mark types from JSON, and wires live data from real IoT devices (or simulated stand-ins) into the visualisation. Implementation of `../../XR_UX-proposal1.md` §5.

Current state: **M22 (2026-05-21)** — four end-to-end use cases shipping, ~244 unit tests, fully documented per-UC manifests under `examples/`.

## Use cases

| | Code | Theme | Notes |
|---|---|---|---|
| **UC1** | `DEMO01` | Personal vitals dataspace | mmWave HR/BR + simulated body-temp; ECG-style line + arc + streamgraph + body-temp line. |
| **UC2** | `DEMO02` | Room dataspace | Live ESP32-CAM, AHT20 temp/humidity, simulated AQI/baro/pollen, in-XR actuator panel for light + strip + thermostat + speaker. |
| **UC3** | `DEMO03` | XRt Exhibit (curated art-data exhibit) | Voronoi stippling (triangular prism), 300° moon-phase arc, force-directed tree (grabbable), ceiling owl tile with spatial mp3 hoots. |
| **UC4** | `DEMO04` | Airplane in-flight experience | Live BMI270 attitude (M5Capsule + Madgwick AHRS + gyro dead-reckoning), procedural cabin music, HLS cabin display, 3-scene Gaussian-splat travel-photo gallery. Mutually-exclusive content modes via show-only HUD. |

Per-UC walkthroughs with device wiring + smoke tests are under `examples/<uc>.md`.

## Documentation

| | When to read it |
|---|---|
| [`DEVELOPER_GUIDE.md`](./DEVELOPER_GUIDE.md) | **Start here** if you want to *create your own dataspace* — manifest authoring, mark types, device wiring, custom builders, deployment. |
| [`API.md`](./API.md) | Full class-by-class library reference. The 1000-line "lookup" doc. |
| [`STATUS.md`](./STATUS.md) | Current implementation status, known issues, **deferred capabilities** (everything the spec set proposes but we haven't built yet). |
| [`CONTRIBUTING.md`](./CONTRIBUTING.md) | How to *extend d3-spatial itself* — adding mark types, palette rules, code style. |
| [`test-plan.md`](./test-plan.md) | Manual on-device walkthrough scripts (5–15 min per UC). |
| [`DESIGN_NOTES.md`](./DESIGN_NOTES.md) | History of key design decisions and their tradeoffs. |
| [`examples/*.md`](./examples/) | Per-use-case walkthroughs (`uc2-room.md`, `uc3-poster.md`, `uc4-airplane.md`). |
| [`XR_UX_BEST_PRACTICES.md`](./XR_UX_BEST_PRACTICES.md) | Lessons learned from on-device testing (Quest 3, Spectacles). |
| `../*.md` (repo root) | The original spec set — `PROPOSAL.md`, `XR_UX-proposal1.md`, `USECASE_SPECS.md`. |

## Quick start

Prerequisites: Node 20+, npm. (Quest 3 or Snap Spectacles for on-device testing; desktop preview works too.)

```bash
git clone <repo>
cd reference-designs/webxrofthings/prototype/d3-spatial
npm install
```

Two processes run in parallel during dev:

```bash
# Terminal 1 — the join-code server (default port 3001)
npm run server

# Terminal 2 — the Vite dev server (port 5173)
npm run dev
```

If port 3001 is taken (e.g., by another NestJS project), override on both ends:

```bash
JOIN_SERVER_PORT=3101 npm run server
JOIN_SERVER_PORT=3101 npm run dev
```

Open in the browser:
- `http://localhost:5173/` → join keypad. Type `DEMO01`–`DEMO04` for the four use cases.
- `http://localhost:5173/?manifest=/examples/uc1-vitals.json` → direct manifest load (skips the keypad).
- `http://localhost:5173/?scene=charts` → demo gallery (renderer-development view).

## On-device testing (WebXR)

WebXR requires HTTPS. The headset's browser refuses `immersive-ar` over plain HTTP.

**Recommended: `cloudflared` tunnel (no signup).**
```bash
brew install cloudflared
cloudflared tunnel --url http://localhost:5173
```
Open the resulting `https://*.trycloudflare.com` URL on the HMD; tap **Start AR**. The URL is ephemeral; rotates per invocation.

**Alternative: `ngrok` (free tier, requires account).**
```bash
brew install ngrok/ngrok/ngrok
ngrok config add-authtoken <YOUR_TOKEN>
ngrok http 5173
```

**Alternative: `mkcert` + local HTTPS (repeatable, no tunnel).** See `examples/uc2-room.md` for the full recipe — needed when you want sub-100 ms LAN latency to a device on the same network as the headset.

## Platform notes

**Meta Quest 3 (primary).** Chromium-based Meta Browser, `immersive-ar` + passthrough out of the box, `local-floor` reference space works. `chrome://inspect` over USB for live debugging.

**Snap Spectacles '24 (secondary).** Optical waveguide, narrow FOV, `local-floor` unreliable (we have a `cam.y − 1.55` fallback + a "Set Floor" toolbar button). Blue text is unreadable through the waveguide — the warm palette in `src/ui/palette.ts` is tuned for this.

**Wolvic (Quest / Pico / Huawei).** Same input model as Quest 3. Hand-tracking varies by host device.

## Using d3-spatial as a library

If you want to consume d3-spatial's primitives (mark builders, Interact system, audio, manifest loader) in your own three.js / WebXR app rather than fork the renderer, the entry points are:

```ts
// Manifest pipeline — the "I have a JSON, give me a scene" path.
import { loadManifest, registerMarkBuilder } from 'd3-spatial/manifest/loader';
import { renderManifestToScene }              from 'd3-spatial/manifest/renderManifest';

// Individual viz primitives — the "I want a 3D streamgraph mesh" path.
import { buildSankey, buildTree, buildForceGraph, buildStreamgraph,
         buildLineMark, buildArcGauge, buildVideoPanel /* … */ } from 'd3-spatial/viz';

// Interaction system — Interact registers any THREE.Object3D as hover/drag-pickable.
import { Interact } from 'd3-spatial/interact/Interact';

// UI chrome — NodeHoverFx (label + halo), VizHud, Breadcrumb, InspectorCard, Toolbar.
import { buildOfflineSensorsHud, buildDemoModeHud } from 'd3-spatial/manifest';

// Audio — spatial hover sounds, procedural music, ambient bed.
import { SpatialHoverAudio } from 'd3-spatial/audio/SpatialHoverAudio';
import { AmbientBed }        from 'd3-spatial/audio/AmbientBed';
```

The project isn't published to npm yet; integration today is by git-submodule or by copying the relevant files into your own project. Full method/class signatures and config options for every primitive are in [`API.md`](./API.md).

The cleanest integration story is **manifest-driven**: write a manifest JSON, hand it to `loadManifest`, attach the result to your scene anchor. That's how all four use cases are built. See [`DEVELOPER_GUIDE.md`](./DEVELOPER_GUIDE.md) for the full walkthrough.

## Project structure

```
src/
  audio/           AudioListener-backed spatial sound + procedural music
  chart/           Chart primitive (line / bar / arc / scatter marks)
  demo/            Reference cells (live HR/BR/IMU/spatial-audio/splat-gallery/actuator-panel)
  interact/        Interact, FingertipGrab, XRBrush, DragBrush
  manifest/        loader, schema, renderManifest, builders, healthMonitor, fakeData, HUDs
  onboarding/      JoinPanel, Keypad, SlotWheel, splashLogo
  ui/              palette, Toolbar, Breadcrumb, NodeHoverFx, InspectorCard, VizHud, DataspaceHud, DataspaceMenu
  util/            tween, easings
  viz/             standalone viz builders — force, tree, treemap, sunburst, pack, sankey, ridgeline, streamgraph, parallel, edgeBundle, tidyTree, tangledTree, voronoiStippling, moonPhasesArc, owlsToTheMax, forceTree3d, videoPanel, glitchTextureShader, globeWidget
  xrRig.ts         WebXR controller + hand-tracking abstraction
  main.ts          App composition

server/            mock-join-server (rotating + fixed DEMO codes), proxy-diag

examples/          The four canonical manifests + per-UC docs
tools/             discover-magnet-devices.mjs (mDNS auto-discovery, see DEVELOPER_GUIDE)
public/            Static assets (Gaussian splats, mp3, jpegs)
```

## Troubleshooting

- **"Code not recognized" on DEMO01-04** — check that `npm run server` is actually bound to its port (a different project on 3001 will intercept silently; we now fail loudly on `EADDRINUSE`). `lsof -i :3001` confirms.
- **Blank scene inside XR** — `local-floor` reference space unavailable; we request `local-floor` as required but soften to optional if needed. Check the HMD's developer console.
- **Audio silent** — WebAudio won't start until a user gesture. Tap once inside the XR session.
- **Camera offline panel** — UC2 falls back to a still image (`fallbackImageUrl` config); if you see the mountain placeholder, the ESP32-CAM isn't reachable. Check `CAMERA_HOST` env / mDNS.
- **DEMO MODE pulsing yellow** — at least one URL-source mark is offline and serving fake data. Hover the red OFFLINE SENSORS card to see which.
- **macOS `.local` resolution issues** — Local Network permission for your terminal app, iCloud Private Relay off, Private Wi-Fi MAC off. See `docs/macOS-LAN-networking.md`.

## References

- Original proposal: [`../XR_UX-proposal1.md`](../XR_UX-proposal1.md)
- XR UX best practices: https://github.com/IoTone/AwesomeSpatialDesign/issues/7
- IoTone UDM/USM spec: `../../specs/UDM-MagNET-v1.md`

## Attributions

- Owl field recording: https://freesound.org/people/depwl9992/sounds/268667/
- Photographic source images (`public/spatial/`): Vincent Foster (Sony A6)
- Stranger Things "neon outline" splash-logo styling, after the Netflix series logo
