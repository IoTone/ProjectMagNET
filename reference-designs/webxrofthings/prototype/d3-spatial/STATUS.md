# d3-spatial prototype — status

Snapshot: 2026-04-18 · Vite + three.js 0.161 + three-mesh-ui 6.5.4 + troika-three-text + Omnitone FOA + d3 core modules.

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

---

## Tooling

- `npm run dev` — Vite dev server, allowed hosts include `*.trycloudflare.com` / `*.ngrok-free.app` / `*.ngrok.app` / `*.ngrok.io` / `*.loca.lt`.
- `npm run smoke` — Playwright-driven headless capture, 79 screenshots across milestones M1.1-M17, writes `demo/shots/*.png` and `demo/index.html`.
- `npm run typecheck` — `tsc --noEmit`, passes.
- `npm run build` — Vite production build, passes.
- Gallery default scene with toolbar (Gallery / Charts / Recenter / Set Floor). `G` key toggles.

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
src/
  main.ts                         session boot, anchor, demo wiring, toolbar, live HR
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
  interact/
    Interact.ts                   hover state machine, desktop + XR raycast, per-hand state, drag
    DragBrush.ts                  pointer-drag brush on charts (desktop)
    FingertipGrab.ts              hand-tracking joint-based grab (index-finger-tip + thumb-tip)
    XRBrush.ts                    XR controller sweep-select (hold-to-brush)
  ui/
    InspectorCard.ts              three-mesh-ui Block + troika Text card, placeNear auto-placement
    NodeHoverFx.ts                halo sphere + billboard label, follows hovered node
    Toolbar.ts                    horizontal button strip (Gallery/Charts/Recenter/Set Floor)
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
    sampleHierarchy.ts            HNode / GraphData / SankeyData types + sample data
    vizGallery.ts                 gallery layout for all 7 spatial marks
  types/
    troika-three-text.d.ts        type shim
    omnitone.d.ts                 type shim for esm + bare module
    d3-force-3d.d.ts              type shim for d3-force-3d
scripts/
  smoke.mjs                       Playwright capture, gallery generator
demo/
  index.html                      auto-generated gallery
  shots.json                      shot manifest
  shots/*.png                     79 screenshots across milestones M1.1-M17
```
