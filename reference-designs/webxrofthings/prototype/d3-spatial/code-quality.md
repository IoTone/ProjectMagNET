# Code Quality & Performance Audit

**Snapshot:** 2026-05-08 (revised; original audit 2026-04-29)
**Scope:** `prototype/d3-spatial/` — 57 TS files, ~10.5k LOC
**Target devices:** Meta Quest 3 (primary), Snap Spectacles, Apple Vision Pro (untested)
**Renderer:** three.js r161 WebGL2; WebGPU evaluation pending

---

## 0. Status update since the original audit

What's landed against this audit's recommendations between 2026-04-29 and 2026-05-08:

| Area | Item | Status |
|---|---|---|
| §2.8 build script | `tsc --noEmit && vite build` (was leaking `.js` next to `.ts` source) | ✅ done |
| §1.x material lifecycle | Per-mark `disposeGroupTree` helper + dispose hooks in line/bar/scatter/arc/streamgraph manifest builders, gallery live-cell `dispose()`, manifest loader `LoadResult.dispose()` wired to pagehide / HMR / `leave-dataspace` action | ✅ done (partial — see §1.2 below) |
| §2.x lighting on dynamic geometry | Switched ridgeline + streamgraph from `MeshStandardMaterial` to `MeshBasicMaterial` (the per-frame `position` updates never recompute normals; the standard material was collapsing to ~black on optical passthrough) | ✅ done |
| §2.x palette discipline | Ridgeline switched to a warm-only palette; line outline uses `TEXT.body` instead of pure white (was invisible on bright passthrough) | ✅ done |
| §1.1 main.ts split | **In progress** — four controllers extracted (`ui/privacyBannerController`, `manifest/manifestController`, `ui/toolbarController`, `audio/ambientController`). main.ts shrunk from >1700 to **1615 LOC**. Pattern established for subsequent slices (dataspace HUD action dispatcher and hierarchy viz drill-in lifecycle are the next big cuts). | 🔄 |
| §2.1 renderer flags | ✅ done — `antialias: false` (rely on XR multisample), dropped `preserveDrawingBuffer`, added `powerPreference: high-performance`, `shadowMap.enabled = false`, `outputColorSpace = SRGBColorSpace`, `xr.setFramebufferScaleFactor(0.9)`. Quest 3 perf delta to be measured against headset; smoke shots verify no desktop visual regression. |
| §2.3 raycast layers / §2.2 alloc pool | Not yet attempted. Remaining perf items; pair well with Quest 3 measurement. | ⏳ |
| Visual regression infrastructure | ✅ done — `scripts/smoke-diff.mjs` + `scripts/smoke-baseline.mjs` with pixelmatch over the 99-PNG baseline; `npm run smoke:diff` is the regression gate. CI workflow `.github/workflows/d3-spatial-ci.yml` runs typecheck/test/build on every push and smoke+diff on PRs. |
| §2.5 Troika text pooling/atlas | Not attempted. Bigger lift than originally hoped because troika doesn't yet target `WebGPURenderer`, so a pure-WebGL pool is still useful. | ⏳ |

The audit's overall posture stands: WebGPU is gated by Troika; the highest-ROI WebGL work is the renderer-flags + raycast-layers + alloc-pool sweep (§2.1–2.3); main.ts urgently needs a split.

---

## 1. Code Quality

### 1.1 Structural

| Severity | Issue | Location | Action |
|---|---|---|---|
| Blocking | `main.ts` is a 1610-LOC god-file mixing scene, interaction, viz wiring, drill state, audio, breadcrumbs, live data, and XR session lifecycle | `src/main.ts` | Split into `scenes/HierarchyVizScene.ts`, `scenes/ChartsScene.ts`, `ui/UIManager.ts`, `scenes/setupInteractions.ts`. Target: each unit <300 LOC. `XRRig` already exists but isn't fully utilized. |
| High | Several `(x as any)` escape hatches around three-mesh-ui and `window.__demo` | `src/main.ts` (~11 sites), `src/onboarding/JoinPanel.ts`, `src/viz/videoPanel.ts:225,228` | Add `Window` augmentation for `__demo` API; thin shim for three-mesh-ui Block. Goal: zero `any` outside `src/types/*.d.ts`. |
| High | TS strictness incomplete | `tsconfig.json` | Already `strict: true`. Add `noUncheckedIndexedAccess`, `exactOptionalPropertyTypes`, `noImplicitOverride`. |
| Medium | No ESLint / Prettier config; copy-paste between mark builders not flagged | repo root | Add `eslint:recommended` + `@typescript-eslint/strict` + `eslint-plugin-import` (catches dup imports, unused vars, `no-floating-promises`). |
| Medium | Heavy duplication across mark builders — palette indexing, hierarchy traversal, focus-path math repeats in `tree.ts`, `treemap.ts`, `sunburst.ts`, `pack.ts` | `src/viz/{tree,treemap,sunburst,pack}.ts` | Extract `src/viz/_hierShared.ts` for: palette-by-depth, descendant-name-path lookup, drill-path resolution. Aim ~200 LOC reduction. |
| Low | No file-level doc strings on viz builders explaining mesh layout, material choice, draw-call count | `src/viz/*.ts` | Single-line JSDoc per builder is enough — what it produces and the perf envelope (e.g. "≤200 nodes; 1 InstancedMesh + 1 LineSegments"). |

### 1.2 Resource Lifecycle (memory leaks)

This is the highest-impact correctness category for a Quest 3 (shared 8 GB; foveated app heap typically <600 MB).

| Severity | Issue | Location | Action |
|---|---|---|---|
| Blocking | Drill rebuilds in `pack.ts`, `tree.ts`, `treemap.ts`, `sunburst.ts` dispose **materials per node** but the shared geometry is correctly retained. However, when the entire mark is rebuilt (e.g. dataspace reload), the **shared geometry is never disposed**. Selection-marker materials are also rebuilt without disposing the previous marker mesh. | `src/viz/pack.ts:59,100`, `src/viz/sunburst.ts:61`, `src/viz/treemap.ts`, `src/viz/tree.ts` | Add `dispose()` returned by every viz builder. Caller (`vizGallery.ts`, `renderManifest.ts`) must invoke on tear-down. Standard contract: `() => { geo.dispose(); materials.forEach(m => m.dispose()); textures.forEach(t => t.dispose()); }`. |
| High | Troika `Text` instances are added/removed in breadcrumb and inspector but never `.dispose()`'d. Each Text owns SDF atlas + geometry. | `src/ui/Breadcrumb.ts`, `src/ui/InspectorCard.ts`, label loops in `src/viz/parallel.ts` | Pool Text objects (recycle on hide) OR call `text.dispose()` on removal. See §2.2 for batching path. |
| Medium | `videoPanel.ts` dispose is mostly correct but `__hlsInstance` is hung off the DOM `<video>` element — fragile if the DOM node outlives the closure | `src/viz/videoPanel.ts:228, 199-211` | Move `hls` into closure scope; the `dispose()` already references it. Drop the `(video as any).__hlsInstance` field. |

### 1.3 Error handling at boundaries

| Severity | Issue | Location | Action |
|---|---|---|---|
| Medium | HLS error handler swallows non-fatal errors with `(_: any, data: any)`; user sees "loading…" forever on CORS/network failure | `src/viz/videoPanel.ts:225` | Type the params (`Hls.Events`, `ErrorData`); surface fatal errors to the status badge with retry. |
| Medium | `loadManifest` failure path not user-visible | `src/manifest/loader.ts` | Verify error surfaces back to JoinPanel; add toast/inline error rather than console-only. |
| Low | Camera proxy / mock-join-server logs to stdout only — no structured error path for production | `server/*.ts` | Acceptable for prototype; revisit before any deployment. |

---

## 2. Performance — WebGL (current renderer)

Target budget: 90 fps on Quest 3 = 11 ms/frame total, ≈4 ms CPU + ≈4 ms GPU per eye after passthrough composition. Below are findings most likely to recover frame-time today.

### 2.1 Renderer configuration

`src/main.ts:33-40`:

```ts
const renderer = new THREE.WebGLRenderer({
  antialias: true,            // ⚠ MSAA expensive on Adreno 740 / Mali G610
  alpha: true,
  preserveDrawingBuffer: true, // ⚠ disables tile-based deferred rendering optimisations
});
renderer.setPixelRatio(window.devicePixelRatio);
```

Recommended:

```ts
const renderer = new THREE.WebGLRenderer({
  antialias: false,                 // rely on XR multisampled framebuffer (MSAA 4x in WebXR layer)
  alpha: true,
  preserveDrawingBuffer: false,
  powerPreference: 'high-performance',
});
renderer.shadowMap.enabled = false; // we don't render shadows
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.xr.setFramebufferScaleFactor(0.9); // dial 0.7–1.0 per device
// Optionally per-session foveation:
//   session.updateRenderState({ fixedFoveation: 1.0 });
```

`preserveDrawingBuffer: true` is the single biggest non-obvious cost on mobile — it forces the GPU to keep the framebuffer post-present, defeating tile-based renderers' eviction.

### 2.2 Per-frame allocations (GC pressure)

`main.ts` and several mark builders allocate `new THREE.Vector3()`/`Quaternion()` inside callbacks invoked per frame (`placeFloorUnderHead`, `repositionDataspaceMenu`, ridgeline / parallel update loops). At 90 fps this churns ≥100 KB/sec on Quest 3 and shows up as periodic 8–12 ms GC pauses.

**Fix pattern** — module-level scratch:

```ts
const _v3a = new THREE.Vector3();
const _v3b = new THREE.Vector3();
const _quatA = new THREE.Quaternion();

function placeFloorUnderHead() {
  const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
  xrCam.getWorldPosition(_v3a);
  floorGrid.position.set(_v3a.x, _v3a.y - EYE_TO_FLOOR_M, _v3a.z);
}
```

Audit targets (do these first):
- `src/main.ts` — `placeFloorUnderHead`, `showVizGallery`, `repositionDataspaceMenu`, drill animation closures
- `src/viz/force.ts` — `tick()` should not allocate
- `src/viz/ridgeline.ts`, `src/viz/parallel.ts` — label re-layout

### 2.3 Raycast cost

`src/interact/Interact.ts:222-237` runs `intersectObjects(scene)` once per visible XR controller plus mouse, with no layer filter. With 100+ scene objects and Troika Text contributing meshes, this is ~250–500 raycasts/sec.

Fix:
1. Define `LAYER_INTERACTIVE = 1`; assign on every hoverable mesh in builders and in `Interact.add()`.
2. `raycaster.layers.set(LAYER_INTERACTIVE)` in `setupRayForHand`.
3. Pass an explicit candidate list to `intersectObjects(this.candidates, false)` instead of `scene.children`. Maintain `candidates: Object3D[]` mirror of `items`.
4. Skip `recursive=true` if step 3 is done.

This typically takes raycast time from ~1.2 ms/frame to <0.2 ms.

### 2.4 Material reuse

Force, pack, sankey, and the chart mark builders each construct fresh `MeshStandardMaterial` instances even when colour/PBR params match a sibling. Renderers cannot batch InstancedMeshes that point at distinct material instances even if uniforms are identical.

Add a tiny cache:

```ts
// src/viz/_matCache.ts
const cache = new Map<string, THREE.Material>();
export function getStdMat(key: string, props: THREE.MeshStandardMaterialParameters) {
  let m = cache.get(key);
  if (!m) { m = new THREE.MeshStandardMaterial(props); cache.set(key, m); }
  return m;
}
```

Disposal contract: caches are app-lifetime; do not dispose entries on viz teardown.

### 2.5 Troika text — biggest single win

Each `Text` instance is its own draw call. Breadcrumb + inspector + parallel-coord labels + viz-gallery sub-labels = 50+ text draw calls. On Quest 3 each draws ~80 µs — 4 ms/frame purely on text.

Two paths, in increasing effort:

**(a) Pool & cull (1 day):**
```ts
class TextPool {
  private free: Text[] = [];
  acquire(): Text { return this.free.pop() ?? new Text(); }
  release(t: Text) { t.text = ''; t.sync(); this.free.push(t); }
}
```
Plus set `text.frustumCulled = true` and verify it actually culls (Troika sets bounding boxes after `sync()`).

**(b) Glyph atlas + InstancedBufferGeometry (3–5 days):**
Build one SDF atlas at startup, render all labels in one `InstancedMesh`. Migrate breadcrumb + inspector first; viz titles can stay on Troika initially. Net: 50 draw calls → 1.

### 2.6 Video texture cadence

`src/viz/videoPanel.ts:183-190` flips `imgTexture.needsUpdate = true` every tick when the `<img>` is loaded — even if the underlying bitmap is unchanged between the 1 fps refreshes. Texture re-upload is real cost (~0.5 ms on Quest 3 for 640×480 RGBA).

```ts
let lastSrcWhenUploaded = '';
tick() {
  if (mjpegImg && imgTexture && mjpegImg.complete && mjpegImg.naturalWidth > 0) {
    if (mjpegImg.src !== lastSrcWhenUploaded) {
      imgTexture.needsUpdate = true;
      lastSrcWhenUploaded = mjpegImg.src;
    }
  } else if (playing && video.readyState >= video.HAVE_CURRENT_DATA && videoTexture) {
    if (video.currentTime !== lastVideoTime) {
      lastVideoTime = video.currentTime;
      videoTexture.needsUpdate = true;
    }
  }
}
```

### 2.7 Frustum culling

InstancedMesh respects `frustumCulled` only if the bounding sphere is correct. After populating instance matrices, call `mesh.computeBoundingSphere()` once. This matters most for force/pack/sankey when the user looks away.

### 2.8 Build / bundle

- **Resolved 2026-04-29 (build-script bug)**: the `build` script was `tsc && vite build`. With no `--noEmit` and no `outDir`, `tsc` emitted `.js` files alongside every `.ts` source, contaminating the source tree (52 files). Vitest's resolver picked the stale `.js` over the current `.ts`, masking newly added exports. Now `tsc --noEmit && vite build`. `vite build` does the actual bundling. (Surfaced while wiring up Phase 1 unit tests — see `test-plan.md`.)
- `npm run build` warns about chunk size. Add `manualChunks` so `viz/*` and `omnitone` lazy-load:
  ```ts
  build: {
    rollupOptions: { output: { manualChunks: {
      d3: ['d3-hierarchy','d3-force-3d','d3-sankey','d3-scale','d3-shape','d3-time','d3-array'],
      omnitone: ['omnitone'],
      hls: ['hls.js'],
    }}}
  }
  ```
- Dynamic-import `omnitone`, `hls.js` (already done for `hls.js`); only load when needed by manifest / user opt-in.

---

## 3. WebGPU readiness

### 3.1 Mark-by-mark feasibility against three.js r161 `WebGPURenderer`

| Mark | WebGPU today | Blocker |
|---|---|---|
| `tree`, `treemap`, `sunburst`, `pack` | Yes (move to `MeshStandardNodeMaterial` or basic) | Migrate from `MeshStandardMaterial` to TSL node material |
| `force` | Yes for render; **huge win** if simulation moves to GPU compute | Sim is currently CPU `d3-force-3d` |
| `sankey`, `parallel`, `edgeBundle`, `tangledTree` | Partial | LineSegments path through TSL is incomplete; lines render but custom width/dash needs work |
| `ridgeline` | Yes | None |
| `videoPanel` | Yes | None — VideoTexture works on WebGPU backend |
| **All Troika text** | **No** | troika-three-text r0.52 does not target `WebGPURenderer`; tracks three.js TSL migration. Watch [troika #285](https://github.com/protectwise/troika) for status. |

**Verdict:** WebGPU migration is gated by Troika. Either (a) replace Troika with our own atlas (§2.5b — would also be a WebGL win) or (b) wait for upstream.

### 3.2 Compute-shader opportunities (high ROI when WebGPU lands)

1. **Force simulation** — Verlet/XPBD compute pass beats `d3-force-3d` by ~50× at n=200. Useful threshold for graph marks.
2. **Sankey iterative layout** — currently O(iters × nodes) on CPU; trivially parallel.
3. **Glyph atlas generation** — only worth it if we ship a custom text path.

### 3.3 Migration roadmap (when we go)

1. Land §2.5b (custom text atlas) — unblocks WebGPU regardless.
2. Replace `MeshStandardMaterial` with TSL node materials in viz builders (mostly mechanical).
3. Add `forceGPU.ts` behind a feature flag; benchmark vs. CPU at n=50/100/200/500.
4. Profile on Quest 3 with WebGPU enabled (Meta Browser supports `webgpu` flag as of 2025).

---

## 4. Triage summary

| Severity | Count | Total effort |
|---|---|---|
| Blocking | 2 (main.ts split, dispose contracts) | 4–6 d |
| High | 5 (raycast layers, allocations, text pool, material cache, renderer flags) | 1–2 wk |
| Medium | 6 (video cadence, error UX, dup viz code, build chunks, frustum bounds, ts strict flags) | 1–2 wk |
| Low | 3 (lint, doc strings, hls cleanup) | 2 d |

**First PR** (highest ROI / lowest risk): renderer flags (§2.1) + raycast layers (§2.3) + allocation pool sweep (§2.2). Expected: +15–20 fps headroom on Quest 3 standalone. Each change is local and reversible.

**Second PR**: dispose contracts (§1.2) — correctness work, no fps gain but unblocks long sessions and dataspace reload.

**Third PR**: main.ts split (§1.1) — enables everything else, including unit tests in `test-plan.md`.
