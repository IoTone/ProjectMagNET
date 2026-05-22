# d3-spatial prototype — status

Snapshot: 2026-04-19 · Vite + three.js 0.161 + three-mesh-ui 6.5.4 + troika-three-text + Omnitone FOA + d3 core modules.

---

## What works

### Render & session (M0)

- WebXR `immersive-ar` boot, `local-floor` reference, passthrough via `alpha=0` clear color on `sessionstart`.
- UI anchor that places 1.2 m in front of the user at eye level (camera y - 0.1 m). No hard-coded floor height.
- Controller ray + 25 mm yellow reticle with halo, `depthTest: false`, renders through everything.
- Desktop fallback: solid-background preview on a non-XR page, full interaction via mouse.
- Floor grid placed under head using `EYE_TO_FLOOR_M = 1.55` heuristic; "Set Floor" button re-calibrates.

### Chart primitive (M1 / M1.1)

- Fluent API: `new Chart().x(scale).y(scale).mark(type).data(arr).render()`.
- Four marks: `line` (tube along CatmullRom), `bar` (InstancedMesh of boxes), `scatter` (InstancedMesh of spheres), `arc` (tube along a 2D curve; used for the TBOC breadcrumb).
- Axis frame auto-built from scales' domains.
- Per-chart invisible "brush plane" for hit-testing drag interactions.

### Interaction (M2 / M4 / M5)

- Hover on any mark via desktop pointer OR XR controller ray -- same state machine, different raycast source.
- Dramatic feedback (per the best-practices issue): 1.05x scale, emissive bump +0.6, white outline box at `renderOrder 995`.
- 150 ms exit debounce prevents flicker when pointer briefly leaves a target.
- Brush selection: drag across any chart to rubberband a translucent cyan rectangle. On release, indices of selected data are returned. Works both via `page.mouse` (smoke) and real pointer drag.
- Inspector card floats adjacent to the hovered mark (three-mesh-ui Block + three troika Text children). Smart auto-placement flips the card to the side with clearance.

### Spatial audio (M6)

- `THREE.PositionalAudio` per mark with procedural sine-tick on hover-in. `AudioListener` re-parented to XR camera on `sessionstart`.
- `AmbientBed` wraps Omnitone FOA renderer; in-memory 4-channel procedural drone as a test bed; rotation matrix updated per frame.
- Visible audio HUD: "♪ ambient: ON · 4ch FOA · HRTF rotating".

### Dataspace federation (M7)

- `DataspaceRegistry` + `DataspaceHud` spatial chip strip with per-dataspace color/glyph.
- Focus dims non-focused marks (scale 0.7, opacity 0.08, emissive suppressed).
- Three demo dataspaces (UC1 wrist, UC2 room, UC3 poster) with marks tagged.

### Gallery visualizations (M8-M11)

Seven spatial marks implemented as a gallery scene accessible via toolbar or `G` key:

- **Tree** (radial node-link) — `d3-hierarchy` cluster layout, InstancedMesh spheres, LineSegments edges, troika billboard labels.
- **Treemap** (extruded city-block) — `d3-hierarchy` treemap layout, InstancedMesh boxes with per-instance color, extrusion height by value.
- **Sunburst** (stacked discs) — `d3-hierarchy` partition layout, ExtrudeGeometry arc segments per node, z-offset by depth.
- **Circular packing** (nested spheres) — `d3-hierarchy` pack layout, individual SphereGeometry meshes with depth-keyed opacity and wireframe root.
- **Force graph** (d3-force-3d) — full 3D physics simulation, InstancedMesh nodes, LineSegments edges, live per-frame tick.
- **Ridgeline** (animated mountain range) — depth-offset density ribbons with time-based animation, fill triangles + line overlay.
- **Sankey** (3D flow tubes) — `d3-sankey` layout, InstancedMesh node boxes, TubeGeometry flow links with radius proportional to value.

### Force graph interaction (M12)

- Per-node hover with NodeHoverFx (halo + billboard label).
- Drag any node: `pinNode(i, worldPos)` sets `fx/fy/fz`, physics keeps running around pinned node.
- Live physics reflow on drag (reheat simulation alpha).

### Tree per-node interaction (M13a)

- Per-node hover with NodeHoverFx.
- Pinch-select (toggleSelected) with translucent selection markers.
- Select on non-leaf triggers drill-in.

### Treemap / sunburst / pack per-node interaction (M13b)

- Per-node/segment hover with NodeHoverFx.
- Toggle-select with selection markers.
- Select on parent-group triggers drill-in.

### Drill-in transitions (M14)

- All four hierarchy marks (tree, treemap, sunburst, pack) support animated drill-in and drill-out.
- Tree and treemap: `tweenInstanced` / `tweenInstancedBoxes` with easeExpOut over 500 ms.
- Sunburst and pack: `tweenMeshes` scale-from-zero animation on rebuild.
- Focus path tracking: `getFocusPath()` / `getFocusLabels()` for breadcrumb trail.

### Multi-hand interaction (M15)

- Per-hand hover state: indices 0, 1 = XR hands; index 2 = mouse.
- Two-hand simultaneous hover + drag on force graph (independent NodeHoverFx per hand).
- Per-hand drag begin/end/move in the Interact state machine.
- Per-hand press lock and select.

### Sankey, ridgeline animation, breadcrumb, per-viz HUD (M16)

- Sankey mark with `d3-sankey` layout, node hover, and flow tubes.
- Ridgeline time-based animation (phase-shifted sample offset per row).
- Breadcrumb trail (three-mesh-ui Block strip) on each hierarchy viz cell, clickable to navigate.
- Per-viz VizHud with Back/Reset buttons; Back visible only when drilled in.
- Manifest schema v1 (`DataspaceManifest`, `MarkSpec`, `MarkType`) covering all 11 mark types plus future types.

### Fingertip grab, XR brush, live streaming (M17)

- **FingertipGrab**: hand-tracking joint-based grab (index-finger-tip + thumb-tip distance < 0.02 m threshold, grab radius 0.025 m). Proximity and grab events fire per hand.
- **XRBrush**: controller sweep-select — hold trigger > 200 ms to enter brush mode, sweep over force nodes to batch-select.
- **Live data streaming**: HR line chart auto-updates every 2 s with synthetic data; `Chart.updateData()` rebuilds geometry with scale domain auto-expansion.

### Layout morph demo (M18)

- `src/demo/morphDemo.ts` — single InstancedMesh for all hierarchy nodes, cycles through four layouts: tree, sunburst, treemap, pack.
- 800 ms tweened transitions (`tweenInstanced` with `easeExpOut`) between layouts; edges hidden during morph, rebuilt on completion.
- "Morph" toolbar button enters/exits morph mode; gallery is hidden during morph mode.
- Auto-cycles every 3 s in morph mode; pinch (or `nextMorph()` hook) advances manually.
- Per-instance color by depth; layout-type label updates on each transition.

### Join-Code Onboarding Phase 1 (M20)

- **JoinPanel** (`src/onboarding/JoinPanel.ts`) — spatial panel with 6-character code entry, "Join" toolbar button opens/closes it.
- **SlotWheel** (`src/onboarding/SlotWheel.ts`) — per-slot character selector with scroll/pointer input.
- **types** (`src/onboarding/types.ts`) — `JoinState` enum and shared onboarding types.
- State machine: `IDLE → ENTERING → SUBMITTING → ACCEPTED / REJECTED`.
- Mock validation on submit; keyboard input wired through for desktop/Bluetooth keyboard.
- Panel dismisses on ACCEPTED; REJECTED shakes and resets to ENTERING.

### Four new section 9 marks (M19)

- **Tidy tree** (`src/viz/tidyTree.ts`) — Reingold-Tilford layout on a cylindrical surface. Nodes wrapped via theta/height -> (cos, y, sin). InstancedMesh spheres, LineSegments edges, troika labels for depth 0-2.
- **Tangled tree** (`src/viz/tangledTree.ts`) — standard tree spine + z-separated CatmullRom tangle arcs. Red (`0xff5577`) for control links, blue (`0x66ccff`) for sync links. Tube radius 1.2 mm, arc depth proportional to level span.
- **Parallel coordinates** (`src/viz/parallel.ts`) — 5 vertical axis rods (CylinderGeometry), 18 data lines across dimensions. Lines colored by group (red/blue/amber). Axis labels at top, scale markers at bottom.
- **Edge bundling** (`src/viz/edgeBundle.ts`) — radial dendrogram (d3-cluster) with leaves on a circle. Graph links routed through hierarchy LCA path, bundled with beta=0.85. CatmullRom tubes colored by source group, max 25 edges for readability.
- Gallery expanded from 7 cells to 4x3 (12 cells) including morph demo.
- Sample data generators: `sampleTangles()` and `sampleParallel()` added to `sampleHierarchy.ts`.

### UC3 "XRt Exhibit" closed (M21) — 2026-05-21

Curated mini-exhibit dataspace built around generative-art / data-art marks. Manifest: `examples/uc3-poster.json`. Per-mark doc: `examples/uc3-poster.md`.

- **Voronoi stippling** (`src/viz/voronoiStippling.ts`) — Bostock's weighted Lloyd-relaxation against an image's luminance grid. Source: `public/spatial/_DSC7796.jpeg`. Downsampled to a 256-px luminance grid (per-iteration walk was 2.5M pixels at native res → 8 FPS for 10 s; 256 keeps it under 5 ms/iter). White stipples (`invert: true`) on a dark panel. `mirrorBack: true` adds a back-facing plane showing the raw photograph at `z=-0.003`, rotated π around Y with `scale.x=-1` to undo the texture mirror — exhibit-walk-around: stippling one side, photograph the other.
- **Moon-phases arc** (`src/viz/moonPhasesArc.ts`) — 29 procedural moons on a 300° arc at radius 2.6 m, height 1.6 m. Custom shader: directional sun vector rotated around the moon's local Y axis by per-moon phase; smoothstep terminator with subtle rim glow on the dark limb. Self-positioned (in `SELF_POSITIONED` set in renderManifest).
- **Force-directed tree** (`src/viz/forceTree3d.ts`) — d3-force-3d, 1 root + 5 clusters + 25 leaves, organic blob layout (no depth-Y bias). Exposes the same `ForceViz`-shaped surface as `force.ts` (`nodeMesh`, `nodes`, `tick`, `pinNode`, `unpinNode`, `reheat`) so renderManifest's nodeMesh registration wires up controller-drag without special-casing. Sim self-ticks via `nodeMesh.onBeforeRender`, alpha-gated so a settled tree costs nothing.
  - **Raycast fixes that landed with the rewrite**: geometry baked at `radius` (was unit-sphere + per-instance scale — the InstancedMesh first-pass cull was rejecting small-scaled instances); `frustumCulled = false`; `boundingSphere.radius *= 2.5` after each recompute so the cull never drops the mesh before per-instance tests run.
- **Owls-to-the-max** (`src/viz/owlsToTheMax.ts`) — canvas-drawn cartoon owl grid, transparent background, mapped to a 2.5 × 1.875 m ceiling plane at world Y 3.2 with one-shot anchor compensation (parent's world Y subtracted on first render so the plane lands at the intended world height regardless of the vizAnchor's per-session position).
  - **Spatial owl hoots**: fetches `public/spatial/268667__depwl9992__owls.mp3` (~30 s field recording), decodes once, and on each scheduled play picks a random 1.0–2.5 s chop at a random offset (`audio.offset` + `audio.duration` passed to `AudioBufferSourceNode.start`). Pool of 4 `PositionalAudio` emitters parented to the cell; each hoot repositions one emitter to a random azimuth at 3–5 m radius with ±0.7 m height jitter, ±12% playback-rate jitter. 2.0–5.0 s gaps between hoots. `dispose()` clears the timer, stops every PositionalAudio and disconnects from the listener graph — leaving the dataspace fully unloads the sound.
- **Manifest pipeline gains**: `loader.ts` parallelised pre-fetches via `Promise.allSettled` with a 5-s `AbortController` timeout and now builds a placeholder mark on failure rather than skipping it (UC2 used to render blank when ENV_HOST was offline; UC3's stippling-from-static-image path is hardened the same way). `renderManifest.ts` wires `onDragStart`/`onDragMove`/`onDragEnd` for any mark whose `viz` exposes `pinNode`/`unpinNode` — the manifest pipeline now supports per-node grab without main.ts special-casing.

### UC4 "Airplane" closed (M22) — 2026-05-21

Real-hardware airplane attitude + cabin music + cabin display + spatial photo gallery. Manifest: `examples/uc4-airplane.json`. Per-mark doc: `examples/uc4-airplane.md`.

- **Airplane attitude (`airplane-imu`)** sourced from a real M5Capsule (ESP32-S3 + Bosch BMI270 6-DoF IMU on I2C G8/G10 at addr 0x69). New firmware: `MagNET_M5DialFiddlerCrab/M5Capsule_Hive_Scribe_Redis/components/craw_imu/` — Madgwick 6-DoF AHRS at 50 Hz, `GET /api/v1/sensor/imu` over `esp_http_server` returning the same JSON shape `liveImuCell.ts` previously polled from `mock-join-server.ts` (so no cell changes were needed for the swap). The bmi270 source is vendored from `espressif/bmi270` 1.0.1~1 (Apache-2.0) — going through the registry transitively pulled in `espressif/sensor_hub` → `espressif/i2c_bus`, and the latter calls `i2c_master_get_bus_handle()` which is ESP-IDF v5.4-only; we're on v5.3.1 via `espressif32@6.9.0`.
  - **`craw_imu` Forth surface**: `imu-on / imu-off / imu-status / imu-zero / imu-scan` (and the i2cdetect-like `sda scl imu-scan` for bus probing — the M5Capsule's BMI270 turned out to be at 0x69 not 0x68, and on G8/G10 not G8/G40 as the original comment had it). `imu-zero` snaps the current pose as HDG 000° (no magnetometer = drifting yaw, this is the user-controlled equivalent of north calibration).
  - **Boot ritual**: 5 ascending buzzer pings 1 s apart as a "lay it flat" reminder, then auto-init IMU + auto-zero after Madgwick converges + two-ping success chime. Triple-low buzz on init failure. Volume controllable via `buzz-v` (one-shot) or `buzz-vol` (default) — piezo duty cycle, max at 50 % (= 128/255). A wifi-monitor task chirps once every 10 s while WiFi is down.
  - **Gyro-driven dead reckoning in `liveImuCell.ts`**: the BMI270 samples at 50 Hz internally but HTTP polling through a cloudflared tunnel is ~2–3 Hz effective. The cell now integrates `angular_velocity` at 60 fps between authoritative polls and snaps to the firmware's fused orientation on each poll. Decouples perceived motion from network latency.
- **Cabin boombox (`cabin-boombox`)**: spatial-audio mark, procedural music generated client-side, looped through `THREE.PositionalAudio` on the scene's listener. Pinch-cycle to advance music theme.
- **Cabin display (`cabin-display`)**: HLS video panel via hls.js, Mux test stream ("Big Buck Bunny"). 720p ABR cap so Spectacles' decoder doesn't choke on 1080p+.
- **Spatial photo gallery (`cabin-spatial-gallery`)**: 3 × Gaussian-splat scenes (compressed.ply, converted from .sog via `npm run convert:spatial`). ArrowL/R navigates, auto-advance every 30 s, splatAlphaRemovalThreshold 60 to fit Quest 3's per-frame triangle budget.
- **HUD switching**: four `show-only:<id>` actions on the bottom HUD — Flight Info / Music / Video / Photos — switch mutually-exclusive content modes. Cells stay loaded but hidden + paused; switching back is instant.

**Wiring details that bit us during bringup** (memos for future-me):
- ESP-IDF httpd's `CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048` is the firmware-side fix for HTTP 431 behind cloudflared (the tunnel injects ~6–10 Cf-*/X-Forwarded-* headers that blow the 512-byte default).
- Vite proxy `/api/v1/sensor/imu` rule MUST sit BEFORE `/api/v1/sensor` — otherwise the UC2 Atom Echo (which has `/api/v1/sensor/environment` but no `/api/v1/sensor/imu`) silently catches the UC4 poll.
- M5Capsule pin map: internal I2C SDA=8 SCL=**10** (not 40 — that's the PDM mic WS), BMI270 at **0x69**, GPIO 46 latches battery power.

---

## Tooling

- `npm run dev` — Vite dev server, allowed hosts include `*.trycloudflare.com` / `*.ngrok-free.app` / `*.ngrok.app` / `*.ngrok.io` / `*.loca.lt`.
- `npm run smoke` — Playwright-driven headless capture, 97 screenshots across milestones M1.1-M20, writes `demo/shots/*.png` and `demo/index.html`.
- `npm run typecheck` — `tsc --noEmit`, passes.
- `npm run build` — Vite production build, passes.
- Gallery default scene with toolbar (Gallery / Charts / Morph / Join / Recenter / Set Floor). `G` key toggles.

---

## Platform support

### Meta Quest 3 (primary)

- Meta Browser (Chromium-based) target. `immersive-ar` with passthrough works out of the box.
- `local-floor` reference space honored. "Set Floor" button for manual calibration.
- Hand tracking 2.0 and Touch Plus controllers both fire standard `selectstart`/`selectend`.
- Full 25-joint hand skeleton via `renderer.xr.getHand(i)` — used by FingertipGrab.
- **Remote devtools:** `chrome://inspect` on desktop for in-headset debugging.
- **Keyboard:** Bluetooth keyboard `G` key toggle for gallery/charts.
- **Performance:** 90 Hz target achievable; force physics tick per frame at 28 nodes is well under budget.
- **Audio:** WebAudio + PositionalAudio + Omnitone all work. First gesture initializes context.

### Snap Spectacles '24 (secondary)

- Optical passthrough constraints: warm amber/cream palette (`src/ui/palette.ts`) tuned for waveguide attenuation.
- No paired keyboard — all control via toolbar or URL query (`?scene=charts`).
- `local-floor` may not be honored; floor grid uses heuristic fallback.
- Debug HUD visible inside UI panel for on-device diagnostics.
- Various visual issues deferred.

### Desktop (full interaction)

- Full mouse-based interaction: hover, click-select, drag-brush, force-drag.
- Smoke test rendering via Playwright headless captures.

---

## Known issues

- ~~Morph demo smoke screenshots appear blank~~ **FIXED** — `morphDemoViz.group` is now re-parented from `galleryRoot` to `vizAnchor` on morph mode entry; smoke screenshots and Quest render correctly.
- ~~Morph blank render on Quest~~ **FIXED** — same re-parenting fix resolves blank frame on device.
- Spectacles floor grid visibility improved but may still need tuning for optical passthrough.
- Quest Touch controllers may show no beam (grip-space fallback may not work for all controller types).
- Spectacles: input may emit via `inputSourceChange` rather than `selectstart/end`. If `ctrls=0` despite visible hands, the `inputsourceschange` path is needed.
- No accessibility layer enforced — audio cues lack guaranteed visible equivalents.
- No persistent user prefs (volume, locale, reduced motion). All state is session-local.
- Bundle size not split — needs code splitting for production.
- `DragBrush` has no XR-controller drag path; on-device chart brushing is desktop-only.
- `hover(null)` / `focusDataspace` must hard-clear all hover feedback (fragile state sync).
- Security boundary for cross-dataspace interaction is visual-only (focus dim); data-layer isolation not enforced.
- Offline cache (UC4) not implemented.

---

## File map

```
src/                                              48 files
  main.ts                         session boot, anchor, demo wiring, toolbar, live HR, morph mode
  xrRig.ts                        controllers + reticle + ray + fingertip sphere, select events, hand joints
  anchor.ts                       (unused after M1; kept for reference)
  chart/
    Chart.ts                      fluent API, mark dispatch, brush, xFractionAt, updateData
    marks/line.ts                 TubeGeometry along CatmullRomCurve3
    marks/bar.ts                  InstancedMesh of boxes
    marks/scatter.ts              InstancedMesh of spheres
    marks/arc.ts                  TubeGeometry arc + tick spheres
  viz/
    tree.ts                       radial/wall tree, drill-in/out, selection, tween
    treemap.ts                    extruded treemap, drill-in/out, selection, tween
    sunburst.ts                   stacked-disc sunburst, drill-in/out, selection, tween
    pack.ts                       circular packing nested spheres, drill-in/out, selection, tween
    force.ts                      d3-force-3d graph, pin/unpin, reheat
    ridgeline.ts                  animated depth-offset density ribbons
    sankey.ts                     d3-sankey layout, flow tubes
    tidyTree.ts                   Reingold-Tilford on cylindrical surface (M19)
    tangledTree.ts                tree spine + z-separated tangle arcs (M19)
    parallel.ts                   5 vertical axis rods, 18 data lines across dimensions (M19)
    edgeBundle.ts                 radial dendrogram with beta=0.85 bundled edges (M19)
  interact/
    Interact.ts                   hover state machine, desktop + XR raycast, per-hand state, drag
    DragBrush.ts                  pointer-drag brush on charts (desktop)
    FingertipGrab.ts              hand-tracking joint-based grab (index-finger-tip + thumb-tip)
    XRBrush.ts                    XR controller sweep-select (hold-to-brush)
  ui/
    InspectorCard.ts              three-mesh-ui Block + troika Text card, placeNear auto-placement
    NodeHoverFx.ts                halo sphere + billboard label, follows hovered node
    Toolbar.ts                    horizontal button strip (Gallery/Charts/Morph/Join/Recenter/Set Floor)
    Breadcrumb.ts                 clickable breadcrumb trail for hierarchy drill navigation
    VizHud.ts                     per-viz Back/Reset buttons, depth-aware visibility
    palette.ts                    warm amber/cream text palette for optical passthrough
  audio/
    SpatialHoverAudio.ts          per-mark PositionalAudio + procedural tick
    AmbientBed.ts                 Omnitone FOA wrapper, load() or loadFromBuffer()
    proceduralBed.ts              in-memory 4-channel FOA drone
  dataspace/
    Dataspace.ts                  Registry + HUD chip strip + applyFocusDim
  manifest/
    schema.ts                     DataspaceManifest, MarkSpec, MarkType types + example
    loader.ts                     loadManifest(), registerMarkBuilder(), data extractors
    builders.ts                   registerAllBuilders() — bridges manifest to viz builders
  util/
    tween.ts                      tweenInstanced, tweenInstancedBoxes, tweenMeshes, easeExpOut
  demo/
    marks.ts                      2x2 demo layout (line, bar, scatter, arc)
    heartRate.ts                  synthetic HR series generator
    sampleHierarchy.ts            HNode / GraphData / SankeyData / ParallelDataPoint types + sample data + sampleTangles + sampleParallel
    vizGallery.ts                 gallery layout for all 12 spatial marks (4x3 grid)
    morphDemo.ts                  single InstancedMesh cycling through 4 hierarchy layouts (M18)
  onboarding/
    types.ts                      JoinState enum, shared types (M20)
    JoinPanel.ts                  join-code onboarding panel (M20)
    SlotWheel.ts                  per-slot character selector (M20)
  types/
    troika-three-text.d.ts        type shim
    omnitone.d.ts                 type shim for esm + bare module
    d3-force-3d.d.ts              type shim for d3-force-3d
scripts/
  smoke.mjs                       Playwright capture, gallery generator
demo/
  index.html                      auto-generated gallery
  shots.json                      shot manifest
  shots/*.png                     97 screenshots across milestones M1.1-M20  (97 files)
```
