# d3-spatial prototype — status

Snapshot: 2026-04-15 · Vite + three.js 0.161 + three-mesh-ui 6.5.4 + troika-three-text + Omnitone FOA + d3 core modules.

---

## What works

**Render & session (M0)**
- WebXR `immersive-ar` boot, `local-floor` reference, passthrough via `alpha=0` clear color on `sessionstart`.
- UI anchor that places 1.2 m in front of the user at eye level (camera y − 0.1 m). No hard-coded floor height.
- Controller ray + 25 mm yellow reticle with halo, `depthTest: false`, renders through everything.
- Desktop fallback: solid-background preview on a non-XR page, full interaction via mouse.

**Chart primitive (M1 / M1.1)**
- Fluent API: `new Chart().x(scale).y(scale).mark(type).data(arr).render()`.
- Four marks: `line` (tube along CatmullRom), `bar` (InstancedMesh of boxes), `scatter` (InstancedMesh of spheres), `arc` (tube along a 2D curve; used for the TBOC breadcrumb).
- Axis frame auto-built from scales' domains.
- Per-chart invisible "brush plane" for hit-testing drag interactions.

**Interaction (M2 / M4 / M5)**
- Hover on any mark via desktop pointer OR XR controller ray — same state machine, different raycast source.
- Dramatic feedback (per the best-practices issue): 1.05× scale, emissive bump +0.6, white outline box at `renderOrder 995`.
- Brush selection: drag across any chart to rubberband a translucent cyan rectangle. On release, indices of selected data are returned. Works both via `page.mouse` (smoke) and real pointer drag.
- Inspector card floats adjacent to the hovered mark (three-mesh-ui Block + three troika Text children). Smart auto-placement flips the card to the side with clearance.

**Dataspace federation (M7)**
- `DataspaceRegistry` + `DataspaceHud` spatial chip strip with per-dataspace color/glyph.
- Focus dims non-focused marks (scale 0.7, opacity 0.08, emissive suppressed).
- Three demo dataspaces (UC1 wrist 👤, UC2 room 🏠, UC3 poster 🏛) with marks tagged.

**Audio (M3 / M6)**
- `THREE.PositionalAudio` per mark with procedural sine-tick on hover-in. `AudioListener` re-parented to XR camera on `sessionstart`.
- `AmbientBed` wraps Omnitone FOA renderer; in-memory 4-channel procedural drone as a test bed; rotation matrix updated per frame.
- Visible audio HUD: "♪ ambient: ON · 4ch FOA · HRTF rotating".

**Tooling**
- `npm run dev` — Vite dev server, allowed hosts include `*.trycloudflare.com` / `*.ngrok-free.app` / `*.ngrok.app` / `*.ngrok.io` / `*.loca.lt`.
- `npm run smoke` — Playwright-driven headless capture, 30 shots across 7 milestone sections, writes `demo/shots/*.png` and `demo/index.html`.
- `npm run typecheck` and `npm run build` — both pass.
- README has tunnel docs (cloudflared / ngrok / mkcert) and troubleshooting.

---

## Known bugs and rough edges

**Meta Quest 3 (primary reliable test platform)**
- Meta Browser (Chromium-based) is the target. `immersive-ar` with passthrough works out of the box.
- `local-floor` reference space is honored, so `camera.y` ≈ eye height above actual floor. The `EYE_TO_FLOOR_M = 1.55` heuristic is harmless but not strictly needed here; the "Set Floor" button is a no-op improvement over the correct default.
- Hand tracking 2.0 and Touch Plus controllers both fire standard `selectstart`/`selectend`. Controller ray path is the most precise; hands are fine for most viz interactions.
- Full 25-joint hand skeleton available via `renderer.xr.getHand(i)` — the roadmap's Level 2 direct-fingertip grab would land here first.
- **Remote devtools:** plug Quest into a desktop, enable Developer Mode, visit `chrome://inspect` in Chromium on desktop — lets you inspect the in-headset page, see console logs, and set breakpoints. This is the single best convenience over any other device we're testing.
- **Keyboard shortcut:** pair a Bluetooth keyboard → the `G` key toggle for gallery/charts works. Makes rapid iteration on scenes much faster than the toolbar buttons. Physical keyboards also let you dump live state from the browser console.
- **Screenshot/record:** the system-level record feature captures passthrough + XR content, good for sharing results. The browser's own screenshot doesn't capture the XR frame — use the system capture instead.
- **Performance:** 90 Hz target achievable; physics tick each frame at 28 force nodes is well under budget. Watch for frame drops when dragging a node (reflow tension increases CPU load).
- **Audio:** WebAudio + PositionalAudio + Omnitone all work. First user gesture initializes the context — tap or pinch once before expecting sound.
- **Limitations shared with Spectacles:** no WebBLE / WebUSB / WebSerial / WebHID in Meta Browser (mirrors the Wolvic gap, see `XR_UX-proposal1.md §7`). IoT device integration from WebXR is not possible without a bridge.

**Snap Spectacles '24 (reported)**
- Initial anchor placement was far overhead before the `Math.max(1.2, pos.y - 0.15)` clamp was removed. Post-fix, expected to spawn at eye level; re-verify.
- Targeting was difficult — added: bigger reticle, always-visible ray, XR controller raycast wired into `Interact`, pinch-in-empty-air to recenter. Re-verify.
- Audio not confirmed on device yet.
- **Keyboard:** Spectacles has no paired keyboard, so `G` shortcut and DevTools console are unavailable. All scene control must go through the toolbar buttons or URL query (`?scene=charts`).
- **Reference space:** local-floor may not be honored — floor grid uses `cam.y − 1.55` heuristic as a fallback. The "Set Floor" toolbar button lets the user re-calibrate if the estimate is off.
- **Text palette:** Spectacles' optical waveguide attenuates blue disproportionately; the warm amber/cream palette (`src/ui/palette.ts`) is tuned for this. Applies equally well on Quest 3 but less critical there.
- Debug HUD (`XR · cam y=… · ctrls=… · hover=…`) is now visible inside the UI panel bottom-right for on-device diagnostics.

**Cross-device uncertainty**
- Spectacles may emit input via `inputSourceChange` rather than standard `selectstart/end` controller events. If `ctrls=0` on device despite visible hands, need the `inputsourceschange` path.
- Listener re-parenting on XR session assumes `renderer.xr.getCamera()` receives head-pose updates; verified under three.js 0.161, not under Spectacles builds.
- Omnitone's FOA renderer has not been exercised on-device; initialization may block on an AudioContext in `suspended` state until first user gesture.

**Architectural rough edges**
- `DragBrush` has no XR-controller drag path; on-device brushing is not wired.
- `hover(null)` / `focusDataspace` must hard-clear all hover feedback because the state machine can be out of sync when the pointer moves during long-running events. Works but fragile.
- `Material.visible = false` vs. `Object3D.visible = false` — raycaster honors the latter, not the former. The invisible brush plane tags itself via `userData.isBrushPlane` and is filtered in hit lists; this convention must propagate to any future invisible hit-targets.
- `Raycaster.params.Line.threshold` default of 1.0 m was making axis `THREE.Line` segments hit-targets from across the room. Tightened to 0.0005 and `isLine`/`isLineSegments` are filtered from hover hits.
- No accessibility layer — every audio cue should have a visible equivalent; we have not enforced this invariant.
- No persistent user prefs (volume, locale, reduced motion). All state is session-local.

---

## What's NOT implemented yet

- Real XR controller brush (drag across a chart with pinch-hold).
- Security boundary for cross-dataspace interaction (currently a focus is just a visual dim; data-layer isolation is not enforced).
- Manifest schema / loader — the demo marks are hard-coded; a dataspace-published manifest would let any of these renderers drop in.
- Offline cache (UC4).
- Any of the hierarchy / graph marks — see `../../XR_UX-proposal1.md` §9 for the design catalog that's next to land.

---

## File map

```
src/
  main.ts                         session boot, anchor, demo wiring
  xrRig.ts                        controllers + reticle + ray, select events
  anchor.ts                       (unused after M1; kept for reference)
  chart/
    Chart.ts                      fluent API, mark dispatch, brush, xFractionAt
    marks/line.ts                 TubeGeometry along CatmullRomCurve3
    marks/bar.ts                  InstancedMesh of boxes
    marks/scatter.ts              InstancedMesh of spheres
    marks/arc.ts                  TubeGeometry arc + tick spheres
  interact/
    Interact.ts                   hover state machine, desktop + XR raycast
    DragBrush.ts                  pointer-drag brush; XR drag not wired yet
  ui/
    InspectorCard.ts              three-mesh-ui Block + troika Text card
  audio/
    SpatialHoverAudio.ts          per-mark PositionalAudio + procedural tick
    AmbientBed.ts                 Omnitone FOA wrapper, load() or loadFromBuffer()
    proceduralBed.ts              in-memory 4-channel FOA drone
  dataspace/
    Dataspace.ts                  Registry + HUD chip strip + applyFocusDim
  demo/
    marks.ts                      2×2 demo layout
    heartRate.ts                  synthetic HR series
  types/
    troika-three-text.d.ts        type shim
    omnitone.d.ts                 type shim for esm + bare module
scripts/
  smoke.mjs                       Playwright capture, gallery generator
demo/
  index.html                      auto-generated gallery
  shots.json                      shot manifest
  shots/*.png                     30 screenshots across 7 milestones
```
