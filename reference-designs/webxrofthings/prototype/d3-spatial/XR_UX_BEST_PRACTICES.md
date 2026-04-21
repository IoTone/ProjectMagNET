# XR UX Best Practices — d3-spatial

Living reference of XR UX lessons learned during d3-spatial prototype development. Organized by category for quick lookup.

---

## Panel & Content Placement

- **Anchor on session start, not at load time.** Place UI relative to the user's head position, not the world origin. Use a 500 ms delay after `sessionstart` to let the XR camera settle before reading head pose.
- **EYE_TO_FLOOR_M = 1.55** for floor estimation when `local-floor` reference space is not honored (Spectacles). The floor grid is placed at `camera.y - 1.55`.
- **Titles and subtitles OUTSIDE cell bounds** (above/below), with z pushed forward (+0.03) so 3D content inside the cell cannot occlude text.
- **Comfortable viewing distance:** 1.2-1.5 m for content panels and gallery cells; 0.8 m for the toolbar (closer because it is a transient action surface, not sustained reading).
- **rowGap between grid rows** (0.10 in the gallery) to prevent title text from one row colliding with subtitle text from the row above.
- **Gallery layout: 4 columns, rowGap-separated.** Cell dimensions 0.38 wide x 0.30 tall. Row gap prevents label overlap.
- **vizAnchor.lookAt(user)** after placement so the gallery plane faces the user, not an arbitrary world direction.

---

## Text & Color for Optical Passthrough

- **NEVER use blue or cyan for text or edges.** Optical waveguides (Spectacles, Quest passthrough) attenuate short wavelengths disproportionately. The human eye's luminance peak is near 555 nm (yellow-green). Cool blues appear faint and unreadable over bright real-world backgrounds.
- **Warm amber palette:**
  - `TEXT.primary` = `0xffd97a` (warm amber) — titles, headers, callouts
  - `TEXT.body` = `0xf5e9c8` (warm cream) — body copy, node labels
  - `TEXT.muted` = `0xb8a380` (muted warm) — subtitles, secondary info
  - `TEXT.dim` = `0x7a6e5a` — disabled, off-state
  - `TEXT.emphasis` = `0xffffff` (pure white) — value readouts
  - `TEXT.accent` = `0x88ff99` (soft mint) — success, active state
  - `TEXT.warn` = `0xffb873` (warm peach) — warning, attention
  - `TEXT.error` = `0xff7a8a` (warm coral) — error
- **Edge/connection lines:** `EDGE.link` = `0xb8a380` (warm mid-tone), NOT `0x3a5a8a` (dark blue). Axis lines: `EDGE.axis` = `0x9a8a70`.
- **Dark backgrounds DISAPPEAR on optical passthrough.** The display adds light but cannot block it. A panel with `0x000000` background is invisible. Use warm dark browns (`0x2a2520`) at 90-95% opacity, not dark blues (`0x0f1a2c`) which look cold and faint.
- **Minimum text fontSize:**
  - `0.011` for labels, sublabels, and small annotations
  - `0.014` for HUD text, status lines, and button labels
  - `0.018` for titles and section headers
  - `0.024` for primary titles on floating panels
  - `0.028` for the main app title/header
- **Use troika-three-text for all text rendering.** three-mesh-ui's built-in text uses MSDF font atlases that only cover ASCII. troika-three-text handles arbitrary Unicode via runtime SDF generation.

---

## Input & Interaction

- **Per-hand state machine.** Each hand (0, 1 = XR hands; 2 = mouse) hovers and drags independently. Do not share hover state across hands.
- **Exit debounce (150 ms)** prevents hover jitter when the hand-tracking ray briefly leaves a target boundary. Without this, hover feedback flickers rapidly.
- **Press-lock:** Snapshot the hovered target at `selectstart` and freeze it until `selectend`. This prevents the hover target from changing during a pinch gesture (the hand moves when you pinch, which shifts the ray).
- **Trigger = activate (select/toggle), NOT recenter.** Recenter is a toolbar button, not the trigger action. Do not overload the primary input.
- **Reticle only on target hit;** hidden when pointing at empty space. A reticle that is always visible trains users to expect interaction everywhere.
- **Thin beam (0.5 mm taper) in warm amber,** NOT a thick tube or 1px line. The beam should be visible but not attention-grabbing. Taper from 0.5 mm at the controller to 0.0 mm at max range.
- **Raycaster.params.Line.threshold = 0.0005.** The default value (1.0) causes axis lines, edges, and other thin geometry to intercept raycasts from enormous distances, making it impossible to hover through a graph.
- **Filter noHover objects from hit results.** Set `userData.noHover = true` on:
  - `isLine` / `isLineSegments` (edges, axes)
  - `isBrushPlane` (invisible brush hit-test planes)
  - Hover outline boxes
  - Any decorative/structural geometry
- **InstancedMesh supports per-instance hover** via `intersection.instanceId`. Register the InstancedMesh as a single Hoverable with `supportsInstances: true`, then dispatch to individual instances in `onHoverInstance`.
- **Node sizes: minimum 7-9 mm** for hand-tracking targeting at 1 m+ distance. Smaller nodes are nearly impossible to select with hand tracking. 5 mm is acceptable for leaves that are view-only.

---

## Controller vs Hand Tracking

- **`getController(i)` = targetRay space** (works for both controller and hand tracking).
- **`getControllerGrip(i)` = grip space** (fallback for Touch controllers; not present in hand-tracking mode).
- **Beam parented to scene, positioned from whichever source is visible** (controller || grip). Check `controller.visible` each frame.
- **XRControllerModelFactory produces odd "planet" shapes** when the controller model cannot be resolved. Removed entirely; the beam IS the controller visual.
- **`renderer.xr.getHand(i)`** for the 25-joint hand skeleton (Quest hand tracking v2). Returns a Group with joints as children.
- **Fingertip grab:**
  - index-finger-tip + thumb-tip distance < 20 mm = pinch
  - Grab radius: 25 mm around the pinch midpoint
  - Check per frame in the render loop

---

## Audio

- **Initialize AudioContext on first user gesture** (click or pinch), never before. Browsers require a user gesture to start AudioContext. Attempting to create one at load time results in a suspended context.
- **PositionalAudio per mark** for hover ticks. `AudioListener` re-parented to the XR camera on `sessionstart` so spatialization follows the headset.
- **Omnitone FOA for ambient beds.** Import from `'omnitone/build/omnitone.esm.js'` (not bare `'omnitone'`), because the bare specifier resolves to the CJS build which does not work with Vite/ESM.
- **No auto-play ambient;** user must opt in. Ambient audio is controlled via a toolbar button or API hook. Starting audio without user consent is both a UX anti-pattern and a browser policy violation.

---

## Performance

- **InstancedMesh for any mark with >10 identical geometries** (force nodes, tree nodes, bar cells, treemap boxes). Individual Mesh objects for each node cause O(n) draw calls and tank frame rate at 100+ nodes.
- **BufferGeometry with `needsUpdate`** for per-frame animation (ridgeline). Modify the position attribute array in place and set `geometry.attributes.position.needsUpdate = true`.
- **d3-force-3d:** tick at render framerate for small graphs (<100 nodes). For larger graphs, decouple simulation to 30 Hz and interpolate positions in the render loop.
- **`tweenInstanced`** for animated layout transitions (800 ms, exponential-out easing). Operates on the InstancedMesh's `instanceMatrix` directly, no per-node Object3D overhead.

---

## Platform-Specific

- **Quest 3:**
  - `chrome://inspect` on desktop for remote devtools (connect Quest via USB or same WiFi).
  - `local-floor` reference space is honored.
  - Both hand tracking and Touch controllers fire `selectstart`/`selectend` through the standard WebXR input source API.
- **Spectacles:**
  - No keyboard, no devtools available on device.
  - `local-floor` is unreliable; always have a fallback floor estimation.
  - Optical passthrough demands the warm palette described above. Colors that look fine on Quest passthrough (which is a video feed displayed on an LCD) may be invisible on Spectacles (which is an optical waveguide adding light to the real world).
- **Vite dev server:**
  - `server.allowedHosts` must include tunnel domains: `['.trycloudflare.com', '.ngrok-free.app', '.ngrok.app', '.ngrok.io', '.loca.lt']` for testing on headsets over HTTPS tunnels.
- **Screenshots:**
  - `preserveDrawingBuffer: true` on the WebGLRenderer for headless screenshot capture. Without this, `canvas.toDataURL()` and Playwright screenshots return blank frames.
