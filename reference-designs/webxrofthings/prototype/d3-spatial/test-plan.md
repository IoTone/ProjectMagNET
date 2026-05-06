# Test Plan

**Snapshot:** 2026-04-29
**Scope:** `prototype/d3-spatial/`
**Status today:** smoke harness only (Playwright-driven screenshot tour). No unit or integration tests.

---

## 1. Where we are

### 1.1 What runs

| Script | Tool | Purpose | Wired to CI? |
|---|---|---|---|
| `npm run typecheck` | `tsc --noEmit` | Type validation | No — clean today |
| `npm run build` | `tsc && vite build` | Production bundle | No — clean today (chunk-size warning, see `code-quality.md` §2.8) |
| `npm run smoke` | `scripts/smoke.mjs` (Playwright in chromium) | Visual milestone tour: ~88 screenshots through M1.1–M21, drives hover/brush/select/drill/audio/dataspace via the `window.__demo` harness | No — manual run |
| `npm run dev` | Vite | Dev server | n/a |
| `npm run server` / `camera-proxy` | tsx | Mock join server / camera proxy | n/a |

### 1.2 What's covered

- **Smoke (manual):** end-to-end paths in headless Chromium — mark rendering, hover/select/brush state, drill transitions, breadcrumb, inspector card placement, ambisonic audio toggle, dataspace focus/dim, multi-hand edge cases via the demo harness, live HR data updates. Output: 88 PNGs + `demo/shots.json` archived under `demo/shots/`. **No baseline comparison** — shots are reviewed manually.
- **Implicit:** `tsc` strict mode catches type errors at build time.

### 1.3 What's *not* covered

| Surface | Why untested | Risk |
|---|---|---|
| `server/mock-join-server.ts` (sequential code rotation, JWT, manifest serving) | No unit tests authored | High — regressions in JWT or rotation grace silently break onboarding |
| `server/camera-proxy.ts` (header stripping, CORS injection) | Same | Medium — works today, but header-strip list is the kind of code that decays |
| `src/util/tween.ts` | Same | Medium — animation feel regressions hard to spot in screenshots |
| `src/manifest/{loader,schema,renderManifest}.ts` | Same | High — JSON parsing surface, public-facing |
| `src/onboarding/JoinPanel.ts` (slot wheel, validation, code entry) | THREE-mesh-ui in JSDOM is awkward | High — user's first interaction |
| Mark builders (`src/viz/*.ts`) | Need WebGL context; JSDOM is no-go | Covered by smoke visually but no math-level checks |
| WebXR session lifecycle, hand-tracking, spatial audio panning | Requires real HMD or emulator (Playwright has no native WebXR support) | Covered by manual on-device review |
| Camera streaming (ESP32-CAM) | Hardware loop | Covered by manual review |
| Visual regression on the smoke shots themselves | No baseline tool wired | High over time — silent UI drift |

Rough coverage estimate: **smoke ≈ 25–30%** of behaviour validated at the visual level. **Logic-level tests: 0%.**

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

### Phase 2 — visual regression on smoke (half day)

Goal: catch unintentional UI drift on the existing 88-shot tour.

- [ ] Commit a baseline set under `demo/shots-baseline/`.
- [ ] Extend `scripts/smoke.mjs` with a `--diff` flag using `pixelmatch` or `odiff-bin`. Threshold: 0.5% pixel diff per shot.
- [ ] On diff: write `demo/shots-diff/<name>.png` and exit non-zero.
- [ ] Add `npm run smoke:diff` script.
- [ ] Document baseline-update flow in `CONTRIBUTING.md` (`npm run smoke -- --update-baseline`).

This is the highest leverage step — every existing milestone shot becomes a regression test for free.

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

---

## 6. Open questions

- **Visual diff tooling**: `pixelmatch` (npm, simple) vs `odiff-bin` (faster, native). Default to `pixelmatch` unless smoke run-time becomes painful.
- **Vitest vs node:test**: vitest has the better DX with our Vite stack; node:test has zero deps. Pick vitest.
- **WebXR emulator**: revisit when Apple Vision Pro support is requested.
