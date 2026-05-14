# Test Plan

**Snapshot:** 2026-05-08 (revised; original 2026-04-29)
**Scope:** `prototype/d3-spatial/`
**Status today:** 68/68 unit + integration tests via vitest, plus the Playwright smoke tour. CI not yet wired. Coverage on covered modules is high (90–100%); aggregate is 45% because builders/loader/liveVitalsCells/renderManifest are still untested. See §5.

---

## 1. Where we are

### 1.1 What runs

| Script | Tool | Purpose | Wired to CI? |
|---|---|---|---|
| `npm run typecheck` | `tsc --noEmit` | Type validation | No — clean today |
| `npm run build` | `tsc --noEmit && vite build` | Production bundle | No — clean today (chunk-size warning, see `code-quality.md` §2.8) |
| `npm test` | vitest | 68 unit + integration tests, ~1 s suite | No — manual run |
| `npm run test:coverage` | vitest + v8 | Coverage report (HTML at `coverage/`) | No |
| `npm run smoke` | `scripts/smoke.mjs` (Playwright) | Visual milestone tour: ~88 screenshots through M1.1–M21, drives hover/brush/select/drill/audio/dataspace via `window.__demo` harness | No — manual run |
| `npm run dev` | Vite | Dev server (proxies join-server, camera, MagNET-Vitals device) | n/a |
| `npm run server` / `camera-proxy` | tsx | Mock join server / camera CORS proxy | n/a |

### 1.2 What's covered

- **Unit + integration (vitest):** `tween`, manifest `schema` + `validateManifest`, `mock-join-server` (code rotation, JWT, rate limit, manifest serving), `camera-proxy` (header strip, CORS), `streamgraph` viz-builder. 68 tests, all green.
- **Smoke (manual):** end-to-end paths in headless Chromium — gallery rendering, hover/select/brush, drill transitions, breadcrumb, inspector card placement, ambisonic audio toggle, dataspace focus/dim, multi-hand edge cases via the demo harness, live HR data updates. Output: 88 PNGs + `demo/shots.json` archived under `demo/shots/`. **No baseline comparison** — shots are reviewed manually.
- **Implicit:** `tsc` strict mode catches type errors at build time.

### 1.3 What's *not* covered

| Surface | Why untested | Risk |
|---|---|---|
| `src/manifest/loader.ts` refresh path | Added 2026-05-08, no test yet | High — manifest refresh is the live-data heartbeat |
| `src/manifest/builders.ts` (line/bar/scatter/arc/streamgraph adapters) | Mostly added 2026-05-07/08; pure transforms but unit-tests not yet authored | High — silent breakage shows up only in browser |
| `src/demo/liveVitalsCells.ts` (HR/BR/phases/targets) | Added 2026-05-05; relies on `fetch` + `setInterval` | Medium — gallery cells silently flat-line on regression |
| `src/onboarding/JoinPanel.ts` (slot wheel, validation, code entry) | THREE-mesh-ui in JSDOM is awkward | High — user's first interaction |
| Other viz mark builders (`src/viz/*.ts`) | Need WebGL context; JSDOM is no-go | Covered by smoke visually but no math-level checks |
| WebXR session lifecycle, hand-tracking, spatial audio panning | Requires real HMD or emulator (Playwright has no native WebXR support) | Covered by manual on-device review |
| Live-device endpoints (MagNET Vitals, ESP32-CAM) | Hardware loop | Covered by manual review |
| Visual regression on the smoke shots themselves | No baseline tool wired | High over time — silent UI drift |

Aggregate v8 coverage: **45%** of statements over included paths (up from 0% baseline; reflects both the original Phase 1 backfill and subsequent feature growth that outpaced new tests).

---

## 2. Target test pyramid

```
┌──────────────────────────────────────────────────────┐
│ E2E (Playwright)                                     │
│   smoke.mjs — already exists (88 shots)              │
│   + visual diff baseline (Pixelmatch / odiff)        │
└──────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────┐
│ Integration                                          │
│   server/mock-join-server  — supertest               │
│   server/camera-proxy      — supertest + nock        │
│   manifest/loader           — fixture JSON           │
└──────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────┐
│ Unit (vitest)                                        │
│   util/tween, ui/palette                             │
│   manifest/schema validators                         │
│   onboarding code generator (extracted from server)  │
│   d3 layout helpers extracted from viz/*             │
└──────────────────────────────────────────────────────┘
```

Pick **vitest** over Jest — it shares Vite's resolver, ESM-native, faster watch, no separate config for TS.

---

## 3. Phased plan

### Phase 1 — fast wins (1 day)

Goal: unit tests for pure logic. Adds `npm test`, `npm run test:watch`.

- [ ] Add devDeps: `vitest`, `@vitest/coverage-v8`, `supertest`, `@types/supertest`.
- [ ] `vitest.config.ts` — point to `src/**/*.{test,spec}.ts` and `server/**/*.{test,spec}.ts`.
- [ ] Tests:
  - `src/util/tween.test.ts` — easing curves, clamp, completion flag.
  - `src/manifest/schema.test.ts` — round-trip parse of `examples/uc2-room.json` + `examples/room-dataspace.json`; reject malformed manifests (missing `version`, unknown `marks[].type`, bad `data.shape`).
  - `server/mock-join-server.test.ts` (supertest):
    - sequential code generator: starts at `AAAAAA`, increments base-30, skips ambiguous chars (`I`/`1`/`O`/`0`).
    - `POST /join` issues JWT; expired/wrong code → 401.
    - Rotation: code at `t=0` still valid through grace window; rejected after.
    - `GET /manifest` requires JWT.
  - `server/camera-proxy.test.ts` (supertest + nock):
    - strips `cf-*`, `x-forwarded-*`, `via`, `forwarded` from outbound request.
    - injects `Access-Control-Allow-Origin: *` on response.
    - 502 if upstream times out (don't hang the client).

Target: ~30 tests, <2 s suite time. Coverage baseline established.

### Phase 2 — visual regression on smoke (landed 2026-05-08)

Every existing milestone shot is now a regression test.

- [~] Baseline lives under `demo/shots-baseline/` (99 PNGs, ~9 MB). **Currently `.gitignore`d** — to be re-added once the canonical baseline is finalized; until then each workstation/CI job regenerates its own. See §6.7 for the CI implication.
- [x] `scripts/smoke-diff.mjs` — pixelmatch + pngjs over current vs. baseline. Per-pixel threshold 0.1, max-diff-ratio 0.005 (0.5% of pixels). Failed shots write a red-overlay diff PNG to `demo/shots-diff/`. Exits 0 on full pass, 1 on regression, 2 on usage.
- [x] `scripts/smoke-baseline.mjs` — promotes the current run into the baseline (run after intentional UI changes; commit the new baseline alongside the code change).
- [x] npm scripts: `smoke:diff`, `smoke:baseline`. Plus `--quiet`, `--threshold=`, `--max-diff-ratio=` flags on diff for CI tuning.
- [x] `demo/shots-diff/` is `.gitignore`d (transient output). `demo/shots-baseline/` is also `.gitignore`d (see §6.7).

Workflow:

```bash
npm run smoke           # produce demo/shots/*.png  (~4 min)
npm run smoke:diff      # compare against baseline  (~5 s)
# if a regression is intentional:
npm run smoke:baseline  # promote new shots into demo/shots-baseline/
git add demo/shots-baseline/
```

Sanity-check verified — 99/99 pass against the seeded baseline at exit 0; an injected wrong image was caught (FAIL with diff PNG written) at exit 1.

### Phase 3 — integration on the manifest path (1–2 days)

Goal: prove the manifest pipeline end-to-end without needing XR.

- [ ] `src/manifest/loader.test.ts` — fetch a fixture manifest via `msw` (or local file URL); validate schema; assert mark builders are invoked with correct specs.
- [ ] `src/manifest/renderManifest.test.ts` — render to a non-XR `THREE.Scene`; assert mark count, group hierarchy, deviceRef/serviceRef linkage.
- [ ] Run all of these in jsdom or node-canvas — none of them should need a GPU. (Mark builders that need WebGL get exercised by smoke, not by these tests.)

### Phase 4 — CI wiring (half day)

- [ ] `.github/workflows/test.yml` — typecheck + unit tests on every push.
- [ ] Smoke + visual diff on PRs only (slower, ~4 min).
- [ ] Upload `demo/shots-diff/*.png` as workflow artifact when smoke fails.
- [ ] Branch protection: require `typecheck` and `unit` to be green; smoke informational until baseline stabilises.

### Phase 5 — deeper coverage (post-MVP)

- [ ] Refactor mark builders so layout math is a pure function returning `{nodes:[...], edges:[...]}` separate from `THREE.Group` assembly (already partly true). Unit-test the layout half against d3 outputs.
- [ ] WebXR emulator for hand-tracking gestures (`@iwer/devui` Chrome extension is the realistic option; not Playwright-native).
- [ ] Apple Vision Pro / Snap Spectacles smoke runs — for now, manual screen-capture into `demo/shots-device/`.
- [ ] Audio: assert that `omnitone` FOA buffer is loaded and panned correctly from a synthetic head pose (testable in node with the audio context mocked).

---

## 4. What we explicitly defer

| Surface | Reason | Revisit when |
|---|---|---|
| Live ESP32-CAM streaming under load | Hardware-dependent; transport already proven manually | Multiple cameras in one dataspace |
| Hand-tracking gesture semantics (pinch / point lock) | Needs HMD; iwer emulator is partial | Apple Vision Pro support lands |
| Multi-user shared presence (V1.7) | Not yet implemented | When the signaling path lands |
| Bundle-size budget enforcement | Today's bundle fits; not a hot issue | After §2.8 chunk-split in `code-quality.md` |

---

## 5. Coverage targets

After Phase 1–3:

| Layer | Target | Measure |
|---|---|---|
| `src/util/`, `src/manifest/`, `server/` | 80% line | `vitest run --coverage` |
| `src/viz/*` layout halves | 60% line | post-refactor (Phase 5) |
| `src/main.ts` and UI surfaces | smoke-only | visual regression baseline |
| Overall | 50% line | reported in CI |

Coverage % is a directional check, not a gate. The smoke + visual-diff baseline is the actual quality bar.

### Phase 1 baseline (landed 2026-04-29)

50/50 unit + integration tests passing, ~4 s suite. `npm test`, `npm run test:watch`, `npm run test:coverage` are wired.

| File | Stmts | Branch | Funcs | Lines |
|---|---|---|---|---|
| `src/util/tween.ts` | 100% | 100% | 90% | 100% |
| `src/manifest/schema.ts` | 97.7% | 87.5% | 100% | 97.7% |
| `server/mock-join-server.ts` | 87.5% | 78.5% | 87.5% | 87.5% |
| `server/camera-proxy.ts` | 80.6% | 71.4% | 100% | 80.6% |
| `src/manifest/loader.ts` | 0% | — | — | 0% (Phase 3) |
| `src/manifest/builders.ts` | 0% | — | — | 0% (Phase 5) |
| `src/manifest/renderManifest.ts` | 0% | — | — | 0% (Phase 3) |
| **Aggregate over included paths** | **58.2%** | **81.7%** | **83.3%** | **58.2%** |

The Phase 1 sweep also caught two real defects in production code:

1. **`shape: 'video'` was missing from `URL_DATA_SHAPES`** in `src/manifest/schema.ts`. The validator initially rejected `examples/uc2-room.json` because the camera mark uses `data.shape: 'video'` but the schema only enumerated `hierarchy / graph / series / distributions / flow`. Fixed by adding `'video'` to both the runtime `URL_DATA_SHAPES` const and the `UrlData.shape` union.
2. **`tsc && vite build` was emitting `.js` files alongside `.ts` source** (no `--noEmit`, no `outDir`), shadowing live source under any resolver that prefers `.js`. Vitest 2.x does, which is how this surfaced. Fixed by switching the build script to `tsc --noEmit && vite build` — `vite build` already does the bundling. 52 stale `.js` files were cleaned up.

### Recent additions (2026-04-30 → 2026-05-08)

Test count grew from 50 → **68/68 passing**. New tests landed alongside new features:

| Module | New tests | Notes |
|---|---|---|
| `src/viz/streamgraph` | 6 (file-level) | Animated streamgraph mark; tests cover construction, scrolling, palette use |
| `src/manifest/schema` | +12 (32 total) | UDM-MagNET v1.0 cross-references, deprecated alias warnings, `_comment`/`_doc` underscore-prefixed annotation tolerance |

Coverage briefly regressed because three substantial new modules landed without tests (builders, loader-refresh, liveVitalsCells). The first of those was backfilled below.

### Builders backfill (landed 2026-05-08)

Added `src/manifest/builders.test.ts` — **16 tests** driving every chart-style adapter (`line`, `bar`, `scatter`, `arc`) plus `streamgraph` through the public `loadManifest()` path. Each builder gets:

- Initial render with a valid synthetic series / distribution
- Empty / placeholder behaviour
- `refresh()` swap verification (asserting the inner `THREE.Group` UUID changes after `refresh(spec)`)

### Loader-refresh backfill (landed 2026-05-08)

Added `src/manifest/loader.test.ts` — **14 tests** with a mocked `fetch` and vitest fake timers. Coverage:

- Initial URL fetch: success, non-OK, network error, WebSocket-skip, inline-pass-through
- `samples: [...]` series wrapper unwrapping
- Bearer-token forwarding
- `refreshInterval = 0` schedules nothing; missing `refresh()` schedules nothing; positive interval ticks per-cadence and calls `mark.refresh(spec)`
- Per-mark independent intervals
- Survives transient refresh failures (404, network error) and recovers
- `LoadResult.dispose()` clears intervals; idempotent on double-call

### Live cells backfill (landed 2026-05-08)

Added `src/demo/liveVitalsCells.test.ts` — **18 tests** for `buildLiveLineCell`, `buildLivePhasesCell`, `buildLiveTargetsCell`. Same `fetch`+fake-timer harness as the loader tests. Coverage:

- All three builders: placeholder visible synchronously, swapped after first fetch, rebuilt on each tick, dispose stops polling
- Line cell: accepts both `{samples:[]}` wrapper and bare array; keeps placeholder on `< 2` samples; survives 404 + network error
- Phases cell: skips swap on empty / single-value distributions; `tick()` is safe before the streamgraph exists; forwards to inner viz once built
- Targets cell: static cone+arc+marker present pre-fetch; visible glyph count matches target count; capped at 3 (`MAX_TARGETS`); positions update across ticks; hides all on fetch error

Also tightened `vitest.config.ts` coverage `include` to only the modules that actually have unit tests — was including `src/manifest/renderManifest.ts` (untested) which masked the real coverage posture; gallery-only viz files (`force.ts`, `pack.ts`, etc.) are excluded for the same reason.

Total **116/116 tests passing** (~1 s suite).

| File | Stmts | Branch | Funcs | Lines | Δ from 2026-04-29 |
|---|---|---|---|---|---|
| `src/util/tween.ts` | 100% | 100% | 90% | 100% | — |
| `src/manifest/schema.ts` | 91.5% | 84.3% | 100% | 91.5% | −6.3 |
| `src/manifest/builders.ts` | **86.2%** | 74.6% | 100% | **86.2%** | **+86.2** |
| `src/manifest/loader.ts` | **90.6%** | 78.6% | 70% | **90.6%** | **+90.6** |
| `src/viz/streamgraph.ts` | **100%** | 93.1% | 87.5% | **100%** | new |
| `src/demo/liveVitalsCells.ts` | **98.2%** | 82.8% | 93.3% | **98.2%** | **+98.2** |
| `server/mock-join-server.ts` | 87.5% | 78.5% | 87.5% | 87.5% | — |
| `server/camera-proxy.ts` | 80.6% | 71.4% | 100% | 80.6% | — |
| `src/manifest/renderManifest.ts` | 0% | — | — | 0% | (still WebGL-bound) |
| **Aggregate over tested paths** | **84.5%** | **81.2%** | **89.0%** | **84.5%** | **+26.3** |

Only `renderManifest.ts` remains uncovered in the tracked scope. It needs either JSDOM with mocked WebGL or a refactor that splits the math from the THREE wiring. Defer until either we adopt headless three (e.g. via `@react-three/fiber` testing utilities) or do the §1.1 main-area refactor that would also touch it.

Out of scope for the current coverage scope (intentionally excluded from `include`):

- `src/viz/{tree,treemap,sunburst,pack,force,sankey,ridgeline,parallel,edgeBundle,tangledTree,tidyTree}.ts` — gallery viz builders, exercised visually by the smoke harness; would need WebGL or headless-three for unit testing
- `src/ui/**`, `src/onboarding/**`, `src/audio/**`, `src/interact/**`, `src/dataspace/**`, `src/chart/**` — UI glue / WebXR-coupled / large surfaces with low return on unit testing
- `src/main.ts` — orchestration; should be split first (§1.1 in code-quality.md)

---

## 6. Manual / on-device testing

The unit suite covers logic; a lot of recent work is interactive UI + WebXR + device-coupled and only a human can sign it off. Run these after a checkout that touches anything below the line.

### 6.1 Quick wide pass — 5 minutes

Run from `prototype/d3-spatial/`:

```bash
npm run typecheck       # → silent
npm test                # → 116/116 passing in ~1 s
npm run build           # → clean, ~2.5 s
npm run smoke           # → 99 PNGs in demo/shots/, ~4 min
npm run smoke:diff      # → 99/99 pass against baseline, exit 0
```

Any non-zero exit is a regression. If `smoke:diff` reports `FAIL` rows, compare `demo/shots-diff/<name>.png` (red overlay) to decide whether the change is intentional. Promote an intentional drift with `npm run smoke:baseline` and commit `demo/shots-baseline/`.

### 6.2 Browser preview — desktop UX (~10 minutes)

Start the dev server: `VITALS_HOST=http://magnet-vitals.local npm run dev`. Open `localhost:5173/`.

| Area | Procedure | Pass when |
|---|---|---|
| **Mouse navigation** | right-drag / wheel / middle-drag | orbit / zoom / pan around the scene |
| **Mouse select** | left-click any toolbar button | button highlights, action fires (gallery/charts/morph/recenter etc.) |
| **Keyboard** | press `g` | gallery toggles |
| **Toolbar** | each of the 6 buttons in turn | each fires its callback; active state badge tracks Gallery/Charts/Morph |
| **Floor + recenter** | click `Set Floor`, then `Recenter` | grid drops to head-relative floor; viz anchor flips in front |
| **Live data cells** | gallery → bottom row | HR/BR placeholder lines visible; phases streamgraph scrolls; targets cell shows cone outline |
| **Hover feedback** | mouse-hover any mark | scale 1.05 + emissive bump + outline; inspector card pops up nearby |

### 6.3 UC1 vitals dataspace via DEMO01 (~15 minutes)

With the device flashed and on the same LAN as your laptop:

1. **Device boot:** `pio run -e esp32c6 -t upload -t monitor`. LED → amber → solid green (self-test pass) → cyan idle. Banner shows `radar: UART0 rx=GPIO17 tx=GPIO16` and `LED: GPIO1`.
2. **Provisioning:** if first flash, BLE-provision via nRF Connect to your WiFi.
3. **mDNS:** at the REPL, `prov-status`. With WiFi up, console prints `[mDNS] http://magnet-vitals.local/ resolved on the LAN`. Confirm: `curl -s http://magnet-vitals.local/vitals | jq .` returns JSON.
4. **Open UC1:** browser → `localhost:5173/`. Type **DEMO01** in the join slots and press Submit.
   - Privacy banner pops up (warm coral border, "Privacy notice" title, "I understand" button).
   - Click "I understand" → banner hides, dataspace marks visible.
   - Verify 5 marks from `examples/uc1-vitals.json`: HR line, BR line, phases streamgraph (scrolling), HR radial gauge, **body-temp line** (simulated, served by mock-join-server, slow wander ~36.5–37.0 °C).
   - **Walk into the radar cone**: device LED flips cyan → blue (presence pulse). Targets cell shows a coral glyph at your position. After ~1 minute, HR line populates with first sample.
5. **Show-privacy HUD action:** click 🔒 in the dataspace HUD → banner re-opens.
6. **Leave-dataspace:** click 🚪 → marks disappear, join panel returns. (Confirms `manifestController.dispose()` clears the active intervals.)
7. **Refresh-interval:** open DevTools → Network. Filter to `/api/v1/vitals/`. Watch HR-history fetch every 30 s and phases fetch every 1.5 s.

(URL-based loader `?manifest=/examples/uc1-vitals.json` still works for direct testing without the join panel.)

### 6.3.1 Fixed UC join codes (~3 minutes, hardware-free)

The mock-join-server resolves `DEMO01–04` to per-use-case manifests. Quick check that all four codes route correctly:

| Code   | Dataspace          | Manifest                | Expected content (placeholder content for unbuilt UCs)             |
| ------ | ------------------ | ----------------------- | ------------------------------------------------------------------ |
| DEMO01 | UC1 vitals         | `uc1-vitals.json`       | Vitals device + 5 marks (HR/BR/phases/targets/body-temp) — body-temp is simulated; rest needs real device |
| DEMO02 | UC2 home auto.     | `uc2-room.json`         | Camera + room temp + AQI/baro/pollen sensors + lighting/thermostat/speaker actuators (state mutable via curl; in-XR controls deferred to P4c) |
| DEMO03 | UC3 poster session | `uc3-poster.json`       | Tree/treemap/sankey marks — pending P2 curated content             |
| DEMO04 | UC4 airplane       | `uc4-airplane.json`     | Airplane attitude (P5a IMU sim) + cabin boombox (P5b procedural-music PositionalAudio) + cabin display (P5c HLS video, Big Buck Bunny test stream from Mux) + spatial-photo gallery (P5d, 3× SOG splats from `public/spatial/`, ArrowL/R navigation, 15-s auto-advance). |

Run `npm run server` (the join server) in a second terminal, then `npm run dev`. From the join panel, type each code in turn and verify:
- Code accepted (green checkmark, "Connected to demo-XXX")
- Manifest renders something visible (even if minimal for UC2/UC4)
- Leave-dataspace returns to the join panel cleanly
- Rotating dev code (printed in the server console) still resolves to the default `room-dataspace.json` for backward compatibility

### 6.3.1.1 UC4 splat-gallery asset setup (one-time)

The three .sog Gaussian-splat photo files are not bundled in git (large binaries, ~30 MB total). Copy them once into `public/spatial/` before DEMO04 will show photos:

```bash
mkdir -p public/spatial
cp ../../../ml-sharp/samples/_DSC1624.sog \
   ../../../ml-sharp/samples/_DSC8994.sog \
   ../../../ml-sharp/samples/medaka_20260112_221836000_iOS.sog \
   public/spatial/
```

Without these files, DEMO04 still loads — the gallery cell just logs a `failed to load /spatial/*.sog` in the console for each missing photo and renders blank between the title and the auto-advance counter.

### 6.3.2 UC2 actuator control via curl (~5 minutes, hardware-free)

DEMO02 includes simulated lighting, thermostat, and speaker actuators. State is mutable via POST while the dataspace is loaded. Requires `npm run server` running.

```bash
# Lighting — read state, dim to 30%, change to red
curl -s http://localhost:3001/api/v1/actuator/light | jq
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"brightness_pct": 30, "color": {"r": 255, "g": 60, "b": 60}}' \
  http://localhost:3001/api/v1/actuator/light | jq

# Light brightness line in the dataspace should drop to 30 within 5s
# (next refreshInterval). Toggle off:
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"on": false}' http://localhost:3001/api/v1/actuator/light | jq
# Line should drop to 0.

# Thermostat — set to 24°C cool mode
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"setpoint_c": 24, "mode": "cool"}' \
  http://localhost:3001/api/v1/actuator/thermostat | jq
# Re-read after 60s — current_c should drift toward 24°C at ~0.1°C/min.

# Speaker — trigger a chime
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"sound_id": "chime"}' \
  http://localhost:3001/api/v1/actuator/speaker/play | jq

# NeoPixel strip — read state, then switch to chase pattern at 80% speed
curl -s http://localhost:3001/api/v1/actuator/neopixel | jq
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pattern": "chase", "pattern_speed_pct": 80, "color": {"r": 255, "g": 80, "b": 0}}' \
  http://localhost:3001/api/v1/actuator/neopixel | jq
# Patterns: solid / breathing / rainbow / chase / twinkle (see available_patterns
# in the GET response). Unknown patterns are rejected silently — prior value stays.

# Turn the strip off without losing the look — `on: false` toggles enable,
# the color/pattern state persists for when it comes back on.
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"on": false}' http://localhost:3001/api/v1/actuator/neopixel | jq
```

(Audio playback + NeoPixel animation are server-side state today — the actuators hold the values without driving the animation. Real sound output / LED frames will come when these get wired to firmware; the in-XR colorwheel + pattern picker UX lands with P4c controls.)

### 6.4 Live cells in the gallery (~5 minutes)

`localhost:5173/` (no `?manifest=`). Press `g` if not in gallery. Bottom row should have:
- **HR · live** (warm coral line, label "MagNET Vitals · 60 min, 1/min")
- **BR · live** (warm peach line)
- **phases · live** (3-channel streamgraph, scrolling)
- **targets · live** (top-down floor map: cone + 1.5 m arc + radar marker)

With device online + presence → live data visible within 1.5 s for phases and targets, within ~3 s for HR/BR (snapshot-polled into a 60-sample client buffer). All four badges should be invisible in this state — the "no chrome when healthy" baseline.

**Status badge (offline indicator)** — corner of each live cell, 3 states:
- **live** → no badge (clean).
- **stale** → small amber dot top-right after a single failed fetch. Comes back to live after 1 success.
- **offline** → red `OFFLINE · 12s` pill (seconds since last success). Triggered by 3+ consecutive errors with no success ever, OR > 6 × refreshMs of silence after a prior success. Recovery to live requires **2** consecutive successes (debounce — single-fetch flaps stay in stale).

Manual badge pass:
1. Device on at start → load demo → all four cells show no badge for at least 10 s of polling.
2. Power-cycle device while demo is open → within ~6 s for `targets` (1 Hz refresh) and ~18 s for HR/BR (3 Hz refresh) the badges escalate to the red `OFFLINE` pill, with the seconds counter ticking up.
3. Plug device back in → badges should drop to stale (amber dot) on the first success, then disappear on the second. Should NOT immediately go from offline → live on a single success.
4. Boot demo with device already off → badges should appear within ~3 × refreshMs (no prior success → 3 errors triggers offline directly, skipping stale).

### 6.5 Audio + ambient HUD (~2 minutes)

In gallery view:
- Bottom-left HUD shows `♪ ambient: off` (dim).
- Trigger via dataspace HUD action `toggle-ambient` (or via demo `__demo.startAmbient()` from console).
- HUD transitions: `loading…` (warm) → `ON · 4ch FOA · HRTF rotating` (mint accent).
- Audible: ambient pad in headphones (mute by default in screenshots).
- Toggle off → `off` again.

### 6.6 On-device (Quest 3 / Spectacles) — 15 minutes when you have a headset

Requires a tunnel: `cloudflared tunnel --url http://localhost:5173`.

**Always test on both Quest 3 AND Snap Spectacles** whenever the `WebGLRenderer` constructor args or any `renderer.xr.*` calls change. The two platforms disagree on what those settings tolerate; Quest 3 alone is not sufficient validation. The current renderer block in `src/main.ts` is the known-good baseline for both — read the comments there before changing it.

| Area | Procedure | Pass when |
|---|---|---|
| **AR session enters (Quest 3)** | tap the AR button on the Quest | scene appears against passthrough; LED-amber renderer-clear no longer shows |
| **AR session enters (Spectacles)** | tap the AR button on Spectacles | scene renders — **not blank**. A blank scene means a renderer/XR setting is Spectacles-incompatible. See `~/.claude/.../memory/project_spectacles_webxr_renderer_quirks.md` for the known offenders (non-1.0 framebuffer scale, `antialias: false`). |
| **Marks visible** | look around the gallery | every mark renders against passthrough — **not** dark/black (validates the MeshStandardMaterial → MeshBasicMaterial fix on streamgraph + ridgeline) |
| **Controller select** | trigger on a mark | hover highlight + select fires the same as desktop |
| **Hand tracking** | pinch-grab a force-graph node | drag works; release returns the node to physics |
| **Radial gauge / streamgraph readability** | look at the UC3 marks against bright passthrough | warm-only palette is legible; no faint blue text |

There is no automated XR test path. Human screen-cap + notes go into `MagNET_Vitals_E4TH/test-logs/<date>-<short-name>.md` if you want a reproducible record.

### 6.7 CI verification — once per workflow change

Push any commit touching `reference-designs/webxrofthings/prototype/d3-spatial/**` to a branch and open a PR. The GitHub Actions workflow `d3-spatial-ci.yml` should:

1. **`verify` job** runs on push + PR: `npm ci`, `typecheck`, `test`, `build`. Should be green in ~3 min with npm cache warm.
2. **`smoke` job** runs on PR only: installs Playwright Chromium, runs smoke + diff. ~4 min.
3. **On smoke failure**, the `smoke-diff-<run_id>` artifact contains both `demo/shots-diff/` (red-overlay diffs) and the new `demo/shots/` for review.

If a workflow run hangs at npm ci, the cache key isn't matching; check `cache-dependency-path` in the YAML.

> **Known gap — visual baseline not in repo.**
> `demo/shots-baseline/` is `.gitignore`d and not currently committed. Until it's re-added, the `smoke` job's `smoke:diff` step has nothing to compare against and will either fail outright (no baseline directory) or compare against a freshly-self-generated baseline (no-op). Treat the smoke job as **render-only** in this state — it confirms the demo loads and produces 99 PNGs, but does **not** catch visual regressions. The `verify` job (typecheck + unit tests + build) is unaffected and is the load-bearing CI signal until the baseline is restored.

### 6.8 Known not-yet-tested surfaces

- **`src/manifest/renderManifest.ts`** — only exercised by the smoke shots; no unit coverage.
- **`src/onboarding/JoinPanel.ts`** — slot-wheel, code entry, validation. Smoke covers the visual; logic is untested.
- **`src/main.ts`** orchestrator — being progressively split into controllers, but the remaining ~1600 LOC is the scene/interact/drill-in spine.
- **WebXR session lifecycle** — `sessionstart` / `sessionend` handlers, hand-tracking pinch semantics, fingertip-grab math.
- **Camera proxy + ESP32-CAM streaming** — hardware loop; manual.
- **MagNET Vitals firmware** (`MagNET_Vitals_E4TH/`) — see [its own test plan](../../../MagNET_M5DialFiddlerCrab/MagNET_Vitals_E4TH/test-plan.md).

---

## 7. Open questions

- **Visual diff tooling**: `pixelmatch` (npm, simple) vs `odiff-bin` (faster, native). Default to `pixelmatch` unless smoke run-time becomes painful.
- **Vitest vs node:test**: vitest has the better DX with our Vite stack; node:test has zero deps. Pick vitest.
- **WebXR emulator**: revisit when Apple Vision Pro support is requested.
