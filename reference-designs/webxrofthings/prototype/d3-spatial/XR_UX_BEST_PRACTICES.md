# XR UX Best Practices — d3-spatial

Living reference of XR UX lessons learned during d3-spatial prototype development (M0 – M22). Organized by category for quick lookup. Sections marked **NEW (M22)** are additions or revisions since the initial draft and reflect what we learned over four end-to-end use cases (UC1 vitals, UC2 room, UC3 exhibit, UC4 airplane), on-device testing on Quest 3 and Snap Spectacles '24, and the polish iterations on offline / DEMO mode / splash / autonomous fallback.

---

## Panel & Content Placement

- **Anchor on session start, not at load time.** Place UI relative to the user's head position, not the world origin. Use a 500 ms delay after `sessionstart` to let the XR camera settle before reading head pose.
- **EYE_TO_FLOOR_M = 1.55** for floor estimation when `local-floor` reference space is not honored (Spectacles). The floor grid is placed at `camera.y − 1.55`.
- **Titles and subtitles OUTSIDE cell bounds** (above/below), with z pushed forward (+0.03) so 3D content inside the cell cannot occlude text.
- **Comfortable viewing distance:** 1.2–1.5 m for content panels and gallery cells; 0.8 m for the toolbar (closer because it is a transient action surface, not sustained reading).
- **rowGap between grid rows** (0.10 in the gallery) to prevent title text from one row colliding with subtitle text from the row above.
- **Gallery layout: 4 columns, rowGap-separated.** Cell dimensions 0.38 wide × 0.30 tall.
- **vizAnchor.lookAt(user)** after placement so the gallery plane faces the user, not an arbitrary world direction.
- **NEW (M22) — Titles belong to the wrapper, not the viz primitive.** When a viz internally renders its own title (UC3 video-panel did this early on), and the cell wrapper also renders one, the two stack and partially overlap. Decide once: the cell wrapper owns title + subtitle; the inner viz only renders the data.
- **NEW (M22) — Self-positioning marks for "hero" content.** For elements that must align with the user's gaze regardless of parent transforms (splash logo, onboarding callouts, the join keypad), do not parent to an arbitrary anchor and rely on its lookAt. Compute the world position each frame from `camera.getWorldPosition()` + a horizontal-only forward vector (`_camFwd.y = 0; _camFwd.normalize()`). Otherwise the parent's lookAt can mix Y into the Z basis and the element ends up below or behind the user.
- **NEW (M22) — Motion before content during onboarding.** Slamming UI onto frame 1 of `sessionstart` is jarring and leaves users staring at the wrong direction. Run a slow horizon-creep (6–7 s) on the splash logo while the user looks up and the camera settles. We use FADE_IN_MS = 6400, SLIDE_DURATION_MS = 7000, APPROACH_DISTANCE = 9.0 m for "The WebXR of Things" hero.

---

## Reference Spaces & Anchoring **NEW (M22)**

- **Spectacles is head-relative; Quest is world-relative.** Their `local-floor` reference spaces resolve differently. Pre-session camera coordinates are reliable on Quest and misleading on Spectacles — any UI anchored to a pose captured before `sessionstart` will float away on Spectacles.
- **Re-anchor on the first valid `sessionstart` frame.** Subscribe to `xr.addEventListener('sessionstart')`, then `requestAnimationFrame` once to let the first XR frame land, *then* read head pose and place world-anchored UI.
- **Narrow FOV (Spectacles) demands closer camera-locked overlays.** What sits at z = −1.5 on a Quest 110° FOV needs z = −1.1 to fit a Spectacles ~50° FOV without clipping. Two distance constants, not one.
- **`Object3D.lookAt` vs `Camera.lookAt` is asymmetric** and a recurring footgun:
  - For an `Object3D`, `lookAt(target)` orients local +Z **toward** the target.
  - For a `Camera`, `lookAt(target)` orients local −Z **toward** the target (cameras face −Z by convention).
  - If you `parent.lookAt(camera)` on a group containing text or a panel, the text faces *away* from the user. Always test on both flavors; consider a `faceCamera(obj, cam)` helper that abstracts the sign.

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
- **Dark backgrounds DISAPPEAR on optical passthrough.** The display adds light but cannot block it. A panel with `0x000000` background is invisible. Use warm dark browns (`0x2a2520`) at 90–95 % opacity, not dark blues (`0x0f1a2c`).
- **Minimum text fontSize:**
  - `0.011` for labels, sublabels, and small annotations
  - `0.014` for HUD text, status lines, and button labels
  - `0.018` for titles and section headers
  - `0.024` for primary titles on floating panels
  - `0.028` for the main app title/header
- **Use troika-three-text for all text rendering.** three-mesh-ui's built-in text uses MSDF font atlases that only cover ASCII. troika-three-text handles arbitrary Unicode via runtime SDF generation.
- **NEW (M22) — Troika outline/stroke widths are *percentages of font size*, expressed as strings.** A numeric `outlineWidth: 0.13` is interpreted as 130 mm of stroke in world units — instant "giant red blob" on a 28 mm-tall hero title. Use `outlineWidth: '3%'`, `outlineBlur: '5%'`, `strokeWidth: '10%'` as strings. This footgun cost us an evening.
- **NEW (M22) — Camera-locked status text needs higher contrast than dataspace text.** HUD chrome (OFFLINE / DEMO MODE badges) competes with the user's foveal attention against whatever is behind it. Use `TEXT.warn` / `TEXT.error` (peach / coral) with a dark-brown backing card at 92 % opacity. Mid-tone amber on transparent fails on bright passthrough backgrounds.

---

## Input & Interaction

- **Per-hand state machine.** Each hand (0, 1 = XR hands; 2 = mouse) hovers and drags independently. Do not share hover state across hands.
- **Exit debounce (150 ms)** prevents hover jitter when the hand-tracking ray briefly leaves a target boundary. Without this, hover feedback flickers rapidly.
- **Press-lock:** Snapshot the hovered target at `selectstart` and freeze it until `selectend`. This prevents the hover target from changing during a pinch gesture (the hand moves when you pinch, which shifts the ray).
- **Trigger = activate (select/toggle), NOT recenter.** Recenter is a toolbar button, not the trigger action. Do not overload the primary input.
- **Reticle only on target hit;** hidden when pointing at empty space. A reticle that is always visible trains users to expect interaction everywhere.
- **Thin beam (0.5 mm taper) in warm amber,** NOT a thick tube or 1px line. The beam should be visible but not attention-grabbing. Taper from 0.5 mm at the controller to 0.0 mm at max range.
- **`Raycaster.params.Line.threshold = 0.0005`.** The default value (1.0) causes axis lines, edges, and other thin geometry to intercept raycasts from enormous distances, making it impossible to hover through a graph.
- **Filter `noHover` objects from hit results.** Set `userData.noHover = true` on:
  - `isLine` / `isLineSegments` (edges, axes)
  - `isBrushPlane` (invisible brush hit-test planes)
  - Hover outline boxes
  - Any decorative/structural geometry
- **InstancedMesh supports per-instance hover** via `intersection.instanceId`. Register the InstancedMesh as a single Hoverable with `supportsInstances: true`, then dispatch to individual instances in `onHoverInstance`.
- **Node sizes: minimum 7–9 mm** for hand-tracking targeting at 1 m+ distance. Smaller nodes are nearly impossible to select with hand tracking. 5 mm is acceptable for leaves that are view-only.
- **NEW (M22) — In-XR keypad beats the floating system keyboard.** Asking users to type a 64-character URL on the Quest virtual keyboard is a friction wall. A 4-character DEMO code (`DEMO01`–`DEMO04`) on a 12-key XR-native keypad → join in under 5 seconds. DEMO codes also double as a no-network demo path: the keypad resolves to a manifest URL the renderer fetches even if `localhost:3001` is down.
- **NEW (M22) — Audible affordances don't require headset audio.** UC4 uses the device's onboard buzzer (M5Capsule) for "IMU calibrated / activated" cues, not headset audio. State-change feedback that comes from the physical device the user is holding *is* more legible than the same beep through headset speakers — it grounds the feedback in the right object.

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
- **No auto-play ambient;** user must opt in. Ambient audio is controlled via a toolbar button or API hook.
- **NEW (M22) — Procedural music tracks scene length, not the file.** UC4's cabin music is procedural (synth chord pad + arpeggios) precisely because we don't know how long the user dwells in a scene. A 3-minute mp3 loops audibly; a procedural bed evolves indefinitely.

---

## Performance

- **InstancedMesh for any mark with >10 identical geometries** (force nodes, tree nodes, bar cells, treemap boxes). Individual Mesh objects for each node cause O(n) draw calls and tank frame rate at 100+ nodes.
- **BufferGeometry with `needsUpdate`** for per-frame animation (ridgeline). Modify the position attribute array in place and set `geometry.attributes.position.needsUpdate = true`.
- **d3-force-3d:** tick at render framerate for small graphs (<100 nodes). For larger graphs, decouple simulation to 30 Hz and interpolate positions in the render loop.
- **`tweenInstanced`** for animated layout transitions (800 ms, exponential-out easing). Operates on the InstancedMesh's `instanceMatrix` directly, no per-node Object3D overhead.
- **NEW (M22) — InstancedMesh raycast needs boundingSphere inflation.** Per-instance transforms move the rendered positions outside the local-space bounding sphere computed at construction. Hover stops working at the edges of the cloud. Inflate the boundingSphere radius ×2.5 and set `frustumCulled = false` on the mesh.
- **NEW (M22) — Cell dispose contract.** Every viz primitive must zero out its three.js resources in a `dispose()` method: geometries, materials, textures, audio buffers, animation handles. Orphaned BufferGeometries accumulate over 2-hour sessions and trigger WebGL context loss on Quest. We test this with `npm test` snapshots that compare counts before/after build → dispose.
- **NEW (M22) — Dead-reckoning between low-rate sensor polls.** A 5 Hz IMU rendered at 60 fps shows visible jumps if you naively snap to each new sample. Forward-step orientation by the cached angular velocity each frame between polls, then snap on the next authoritative reading. UC4's airplane viz integrates gyro 12× per IMU poll.
- **NEW (M22) — Per-device proxy with `maxSockets: 1`.** Small ESP chips (C3, C6, ESP32-CAM) serialize HTTP; their httpd reports `sock_err send:11` and a WiFi-task watchdog under overlapping polls. Enforce serialization at the Vite proxy with a per-host `http.Agent({ maxSockets: 1 })`, not in firmware. The fix lives in `vite.config.ts`'s `discoverHostsSync` + agent pool.

---

## Resilience & Data Quality **NEW (M22)**

A dataspace renders the *external world*. The external world disconnects, drifts, lies, and goes home for the weekend. The user must always know what they're looking at.

- **Three-state health model per URL-source mark.** `live` (recent successful fetch) → `stale` (1+ failures, no new data but last value is plausible) → `offline` (consecutive failures over threshold). Implementation: `src/manifest/healthMonitor.ts`. Constants: `INITIAL_OFFLINE_AFTER_FAILS = 1`, `OFFLINE_AFTER_FAILS = 3` (after a prior success), `RECOVERY_SUCCESS_COUNT = 2`.
- **Pulsing DEMO MODE badge** when *any* mark is in `offline` state and fake data is being synthesized. `src/manifest/demoModeHud.ts` renders a yellow pulsing badge in the corner of the user's view. Pulse cadence ~1.2 s. Yellow, not red — DEMO is operational, not broken.
- **OFFLINE SENSORS card** lists every offline mark by name + last-seen timestamp. Red-outlined, click-through pop-out. So the user (and the dev next to them) can immediately answer "what specifically died?"
- **Fake-data fallback must read the chart's value range, not a generic 0–100.** UC2 pollen runs 0–12; a generic 0–100 sine overflowed the chart 7.5×, producing a giant S-shape across the room. `fakeRangeFor(spec)` reads `config.vMin` / `config.vMax` first, falls back to id-keyword heuristics, then to 0–100 only as last resort.
- **Autonomous mode after extended offline.** UC4's IMU goes autonomous after 10 s of fetch failures (`AUTONOMOUS_AFTER_OFFLINE_MS = 10_000`) — synthesizes orientation via three sines of mutually-prime periods (17 / 23 / 41 s) so the airplane keeps moving plausibly for hours. The badge pulses to indicate "I am making this up."
- **Test the offline path explicitly.** Tooling: `MAGNET_DISCOVERY=0 npm run dev` disables mDNS discovery so every URL-source goes offline immediately; pair with the DEMO mode HUD to validate fallbacks.
- **Fake-data determinism.** Seed waveforms by `markId` so two sessions of the same dataspace produce identical fake patterns. Screenshot regression tests remain stable; consecutive ticks line up smoothly without jumps when the cell rebuilds.

---

## Networking & Latency **NEW (M22)**

- **`CONFIG_HTTPD_MAX_REQ_HDR_LEN = 2048`** on every ESP-IDF device the manifest fetches from. Cloudflared injects ~15 `Cf-*` and `X-Forwarded-*` headers; the IDF httpd default (512 B) returns 431 under tunnel and you see "GET /api/v1/… 431" in DevTools with no body. Bump on device, AND strip on the Vite proxy.
- **Vite proxy header strip-list** must include `Cf-*`, `X-Forwarded-*`, `Cdn-Loop`, anything else the tunnel adds. `vite.config.ts`'s `configure: (proxy) => { proxy.on('proxyReq', (req) => { req.removeHeader('cf-connecting-ip'); … }) }`.
- **mDNS auto-discovery** beats hard-coded IPs for LAN dev. `tools/discover-magnet-devices.mjs` wraps `dns-sd -Z`; `vite.config.ts` calls it synchronously at startup. Precedence: env var → mDNS → `.local` fallback. `MAGNET_DISCOVERY=0` kill-switch when scanning hangs in flaky LAN.
- **fetch timeouts via `AbortController`.** A device that crashes mid-poll leaves the fetch hanging; the next poll piles up; soon you have N concurrent stalled fetches. Use `AbortController` + `setTimeout(2000)` per request; pair with the health monitor so timeouts count as failures.
- **macOS LAN gotchas surface as silent failures.** Local Network permission, iCloud Private Relay, Private Wi-Fi MAC randomisation all break `.local` resolution from a Mac with no error message. The fix is documented in `docs/macOS-LAN-networking.md`; mention it on the first sign of "device pingable from terminal but not browser."
- **NEW (M22-late) — Polling cadence calibration.** Default for ambient sensors (temp, humidity, baro, AQI, pollen, light): **30–60 seconds**, not 5. Room temperature changes by ~0.1 °C per minute; polling 12× per minute is pure waste and on small chips it drives the esp_http_server into edge cases. Picked from the 2026-05-25 debug session where `/humidity/history` returned 100 % `431 Request Header Fields Too Large` from browser (but 100 % 200 from direct curl) — the only thing that resolved it was dropping cadence from 5 s → 30 s and offsetting humidity 15 s behind temperature.
- **NEW (M22-late) — Stagger same-cadence polls on the same chip.** Two marks at `refreshInterval: 30` fire simultaneously by default — `Promise.allSettled` at manifest pre-fetch, then `setInterval` ticking together every 30 s. On a tiny chip like the M5 Atom Echo, simultaneous requests serialised by the proxy's `maxSockets: 1` queue right behind each other and the httpd parser can fall over. Use the manifest's `data.startDelayMs` field to offset the *first* scheduled tick — same cadence, different phase. Pre-fetch at `t=0` is unaffected; only the `setInterval` is delayed. Three marks at 30 s? Stagger 0 / 10000 / 20000 → one poll every 10 s instead of three at once every 30. See `examples/uc2-room.json` for the canonical `room-temp` + `room-humidity` setup.

---

## Platform-Specific

- **Quest 3:**
  - `chrome://inspect` on desktop for remote devtools (connect Quest via USB or same WiFi).
  - `local-floor` reference space is honored.
  - Both hand tracking and Touch controllers fire `selectstart` / `selectend` through the standard WebXR input source API.
- **Spectacles '24:**
  - No keyboard, no devtools available on device.
  - `local-floor` is unreliable; always have a fallback floor estimation (`camera.y − 1.55`).
  - **Reference space is head-relative** (see §Reference Spaces). Re-anchor on `sessionstart`.
  - **Narrow FOV** (~50°) — push camera-locked overlays closer; budget for less peripheral content.
  - Optical passthrough demands the warm palette described above. Colors that look fine on Quest passthrough (which is a video feed displayed on an LCD) may be invisible on Spectacles (which is an optical waveguide adding light to the real world).
  - **`setFramebufferScaleFactor` ≠ 1.0 blanks the scene** on Spectacles "Start AR." Likely also true of `antialias: false`. Test renderer changes on both platforms.
- **Wolvic (Quest / Pico / Huawei):** same input model as Quest; hand-tracking varies by host device.
- **Vite dev server:**
  - `server.allowedHosts` must include tunnel domains: `['.trycloudflare.com', '.ngrok-free.app', '.ngrok.app', '.ngrok.io', '.loca.lt']` for testing on headsets over HTTPS tunnels.
- **Screenshots:**
  - `preserveDrawingBuffer: true` on the WebGLRenderer for headless screenshot capture. Without this, `canvas.toDataURL()` and Playwright screenshots return blank frames.

---

## Patterns we keep reaching for **NEW (M22)**

Cross-cutting design moves that recurred across UC1–UC4. Not strict rules — but if a new use case ignores all of them, that's worth a second look.

1. **Show-only HUD for mutually-exclusive scenes.** UC4 has three scenes (cabin / window / gallery) that should never coexist. Instead of toggle state per scene, the HUD exposes a "show X, hide all others" command. One source of truth for "which scene is active"; impossible to enter a both-on state.
2. **Manifest-driven everything.** No demo lives in code; every demo is a JSON file under `examples/`. The renderer is a viewer for a data format, not a collection of hand-built scenes. Builder pattern (`registerMarkBuilder`) keeps that contract honest.
3. **Self-positioned hero marks vs. parent-anchored body marks.** The splash logo, join keypad, and OFFLINE SENSORS card compute their own world transform each frame. Data cells inside a dataspace are parent-anchored and laid out as a grid. Don't mix the two for one element.
4. **Fallback before live.** UC2's camera panel defaults its panel material to the mountain placeholder at construction; the first successful live frame replaces it. The user never sees a black box waiting for an `onerror` to fire 30 s later.
5. **Audible state changes from the physical device, not from headset audio.** UC4's M5Capsule buzzer for "IMU armed." UC2's M5Atom click for actuator-panel taps. Grounds the affordance in the right object.
6. **Health monitor + DEMO mode + autonomous fallback** is the canonical trust stack. Use all three for any sensor-bearing dataspace.

---

## See also

- `D3_GAP_ANALYSIS.md` — what the prototype doesn't yet do, ordered by leverage.
- `DEVELOPER_GUIDE.md` — how to author your own dataspace.
- `DESIGN_NOTES.md` — narrative history of specific design decisions.
- `docs/macOS-LAN-networking.md` — the three macOS network-privacy footguns.
