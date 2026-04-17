# d3-spatial — Milestone 0

WebXR scaffold for the spatial D3 prototype described in `../../XR_UX-proposal1.md` §5.

## What's here (M0)

- Vite + TypeScript + three.js + three-mesh-ui + troika-three-text + d3 core modules (deps declared, not all wired yet).
- WebXR `immersive-ar` session with `local-floor` reference space.
- Passthrough clear color (`alpha = 0`) per the best-practices issue.
- UI anchor group that is re-positioned ~500 ms *after* session start, relative to the user's headset — not at world origin.
- Controller raycaster with a world-space reticle (12 mm sphere, `depthTest:false`, `renderOrder:999`) — the pattern recommended for hand-tracked devices.
- Placeholder cube + troika label attached to the anchor as proof-of-life.

## What's NOT here yet

- The `Chart` primitive (M1).
- `three-mesh-ui` panel integration (M3) — the dep is installed but no Block is mounted yet.
- Hover / brush / inspector interactions (M2).
- Any real d3 scale binding — those modules are declared deps so M1 can begin immediately.

## Running

WebXR requires a secure context. Options:

```bash
npm install
npm run dev        # http://localhost:5173 on desktop for quick checks
```

### On-device testing (Quest, Wolvic, Pico, Spectacles)

WebXR requires a secure context. The HMD will refuse to start an AR session over plain HTTP. Pick one of these paths.

**Option A — `cloudflared` (fastest, no account).**

```bash
brew install cloudflared          # or equivalent for your OS
npm run dev                       # in one terminal
cloudflared tunnel --url http://localhost:5173   # in another
```

`cloudflared` prints an `https://*.trycloudflare.com` URL. Open it in the HMD browser, tap **Start AR**. No signup; the URL is ephemeral and rotates per invocation.

**Option B — `ngrok` (requires free account).**

```bash
brew install ngrok/ngrok/ngrok
ngrok config add-authtoken <YOUR_TOKEN>
npm run dev
ngrok http 5173
```

Use the `https://*.ngrok-free.app` URL.

**Option C — `mkcert` + local HTTPS (best for repeatable testing).**

```bash
brew install mkcert
mkcert -install
mkcert localhost 192.168.x.x <your-lan-ip>
```

Then edit `vite.config.ts` to point at the generated `.pem` files and set `server.https = { key, cert }`. HMD must be on the same LAN; reach the dev server at `https://<your-lan-ip>:5173/`. Root CA must be trusted by the HMD browser — on Quest this usually means sideloading, which is why A/B are preferred for quick iteration.

### Platform-specific notes

**Meta Quest 3 (most reliable).** Meta Browser (Chromium-based) handles `immersive-ar` with passthrough out of the box. `local-floor` is honored — floor grid lands correctly without needing the "Set Floor" button. Controllers and hand tracking both fire standard `selectstart`/`selectend`. **Best debugging path:** enable Developer Mode on the Quest, connect USB, open `chrome://inspect` in desktop Chromium → live page inspection with console, breakpoints, and DOM view while you wear the headset. Pair a Bluetooth keyboard to use the `G` key gallery/charts toggle and to type into the browser console. System-level screen recording captures the passthrough + XR composite for sharing.

**Snap Spectacles '24.** Optical waveguide with narrow FOV. `local-floor` reference space behavior is unreliable — the `cam.y − 1.55` fallback and "Set Floor" button exist for this device. No keyboard pairing, so scene switching goes through the toolbar only (or the `?scene=charts` URL query). Blue text is unreadable through the waveguide — the warm palette in `src/ui/palette.ts` is tuned for this device and matters here more than on Quest.

**Wolvic (Quest / Pico / Huawei).** Same `selectstart`/`selectend` model as Quest 3. Hand tracking support varies by host device.

### Troubleshooting

- **"AR NOT SUPPORTED" button on HMD** — the device didn't advertise `immersive-ar`. Quest needs OS-level passthrough permission for the browser; Wolvic needs `navigator.xr`. Verify in the HMD browser's console.
- **Blank scene inside XR** — `local-floor` reference space may not be available. Fall back to `local`. Our code requests `local-floor` as required; soften to optional if needed.
- **Audio silent** — WebAudio won't start until a user gesture. Tap once inside the XR session (the controller trigger counts).
- **Hand-tracked pinch not registering** — Wolvic hand tracking currently routes pinch through `selectstart`. If this changes, the XRRig wires the same event. On Quest, both hands AND Touch Plus controllers emit `selectstart`; the controller path is more precise for pointing.
- **`ctrls=0` in debug HUD** — means no XR input sources are routed as `XRController` instances. Quest 3 should report 2 immediately when hands or controllers are active. Spectacles may route through `inputsourceschange` only; that path is not yet wired.
- **Node drag on force graph doesn't follow the hand** — verify the reticle lands on a node first (yellow sphere on the node surface). Pinching without a hovered node triggers recenter, not grab.

Desktop preview shows a 2D camera view with the grid + anchor so you can verify scene assembly without a headset.

## Next step (M1)

Add `src/chart/Chart.ts` exposing:

```ts
const chart = new Chart()
  .x(scaleTime().domain([t0, t1]).range([0, 0.4]))
  .y(scaleLinear().domain([0, 200]).range([0, 0.1]))
  .mark('line')
  .data(series);
uiAnchor.add(chart.object3D);
```

First mark: `line` as a `TubeGeometry` along a `d3-shape.line()` polyline — the UC1 wrist heart-rate ribbon.

## References

- Proposal: `../../XR_UX-proposal1.md`
- XR UX best practices: https://github.com/IoTone/AwesomeSpatialDesign/issues/7
