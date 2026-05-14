/**
 * Live splat gallery cell — UC4 spatial-photo viewer.
 *
 * Renders one Gaussian-splat photo from a list, with next/prev navigation
 * to cycle through. Uses `@sparkjsdev/spark@^0.1.10` for SOG/PLY/SPZ
 * loading; the SplatMesh integrates into a regular three.js scene without
 * needing a separate SparkRenderer in basic usage (per spark's README).
 *
 * UX shape:
 *   - One active splat shown at a time
 *   - Slow auto-rotation around Y so the volumetric reading is obvious
 *   - Counter text below ("Photo 2 of 3 · DSC 1624")
 *   - Auto-advance every `autoAdvanceMs` (default 15 s), pauses when paused
 *   - Keyboard left/right for manual navigation (window-level listener)
 *
 * Future polish (deferred):
 *   - Click-to-advance arrows in 3D space (requires onSelect wiring in
 *     renderManifest, which currently only handles hover for manifest-built
 *     marks; that's the same blocker the boombox transport controls hit)
 *   - Thumbnail strip rendered from the .webp companion files
 *   - Cross-fade between splats instead of swap-on-load
 */
import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';

// `@sparkjsdev/spark` is lazy-loaded the first time a splat mesh is needed.
// Reasons:
//   - Spark imports a vrButton helper at module-load that touches `navigator`,
//     which crashes node-env unit tests when this module is imported via the
//     manifest builder chain.
//   - It's a sizable library (~MB-ish). Loading on-demand means non-UC4
//     dataspaces don't pay for it.
// The dynamic import returns a typed singleton so subsequent loads reuse it.
type SparkModule = typeof import('@sparkjsdev/spark');
type SparkSplatMeshCtor = SparkModule['SplatMesh'];
let sparkPromise: Promise<SparkSplatMeshCtor> | null = null;
function loadSparkSplatMesh(): Promise<SparkSplatMeshCtor> {
  if (!sparkPromise) {
    sparkPromise = import('@sparkjsdev/spark').then(m => m.SplatMesh);
  }
  return sparkPromise;
}
type SparkSplatMesh = InstanceType<SparkSplatMeshCtor>;

export interface SplatPhoto {
  /** URL the SplatMesh loader will fetch. Typically `/spatial/foo.sog`. */
  url: string;
  /** Display label shown under the splat. */
  title: string;
}

export interface LiveSplatGalleryOpts {
  photos: SplatPhoto[];
  /** Auto-advance interval in ms. 0 disables. Default 15000. */
  autoAdvanceMs?: number;
  /** Continuous Y rotation rate (rad/sec). 0 disables. Default 0.15. */
  rotateRadPerSec?: number;
  /** Y position of the splat group relative to the cell origin. Default 0. */
  splatY?: number;
  /** Hookup of keyboard left/right for manual navigation. Default true. */
  bindKeyboard?: boolean;
}

export interface LiveSplatGalleryCell {
  group: THREE.Group;
  next(): void;
  prev(): void;
  goTo(index: number): void;
  currentIndex(): number;
  tick(time: number): void;
  dispose(): void;
}

export function buildLiveSplatGalleryCell(opts: LiveSplatGalleryOpts): LiveSplatGalleryCell {
  const {
    photos,
    autoAdvanceMs = 15_000,
    rotateRadPerSec = 0.15,
    splatY = 0,
    bindKeyboard = true,
  } = opts;

  if (photos.length === 0) {
    throw new Error('liveSplatGalleryCell: at least one photo required');
  }

  const group = new THREE.Group();
  group.name = 'live-splat-gallery';

  // The splat hangs off this inner group so we can rotate it without
  // affecting the label position below.
  const splatHolder = new THREE.Group();
  splatHolder.name = 'splat-holder';
  splatHolder.position.y = splatY;
  group.add(splatHolder);

  // Counter label below the splat.
  const label = new Text();
  label.fontSize = 0.018;
  label.color = TEXT.muted;
  label.anchorX = 'center';
  label.anchorY = 'top';
  label.position.set(0, splatY - 0.25, 0);
  group.add(label);

  let currentIdx = 0;
  let currentMesh: SparkSplatMesh | null = null;
  let lastTickTime = 0;
  let disposed = false;

  function updateLabel() {
    const p = photos[currentIdx]!;
    label.text = `${currentIdx + 1} / ${photos.length}  ·  ${p.title}`;
    label.sync();
  }

  function disposeMesh(mesh: SparkSplatMesh | null) {
    if (!mesh) return;
    splatHolder.remove(mesh);
    // SplatMesh holds a PackedSplats / textures internally. Spark exposes
    // a `dispose()` method via SplatGenerator; call defensively in case the
    // typings are missing it on a future version.
    const m = mesh as unknown as { dispose?: () => void };
    m.dispose?.();
  }

  /** Async — the first call lazy-loads `@sparkjsdev/spark`. Each call swaps
   *  the visible splat for the photo at `currentIdx`. Concurrent calls
   *  (e.g. user spamming next()) are guarded by re-checking that we're
   *  still the current target after the load resolves. */
  async function loadCurrent() {
    const photo = photos[currentIdx]!;
    const targetIdx = currentIdx;
    const prev = currentMesh;
    updateLabel();
    let SplatMeshCtor: SparkSplatMeshCtor;
    try {
      SplatMeshCtor = await loadSparkSplatMesh();
    } catch (err) {
      console.error('[splat-gallery] failed to load @sparkjsdev/spark', err);
      return;
    }
    if (disposed || currentIdx !== targetIdx) return;
    const next = new SplatMeshCtor({ url: photo.url });
    currentMesh = next;
    splatHolder.add(next);
    // Free the previous splat once the new one's data is in flight so we
    // never have two large splat textures resident simultaneously. Spark's
    // SplatMesh exposes an `initialized` promise that resolves once the
    // file is fetched + parsed.
    next.initialized
      .then(() => { if (prev && prev !== currentMesh) disposeMesh(prev); })
      .catch((err: unknown) => {
        console.error('[splat-gallery] failed to load', photo.url, err);
        if (prev && prev !== currentMesh) disposeMesh(prev);
      });
  }

  function goTo(index: number) {
    if (disposed) return;
    const wrapped = ((index % photos.length) + photos.length) % photos.length;
    if (wrapped === currentIdx && currentMesh !== null) return;
    currentIdx = wrapped;
    void loadCurrent();   // fire-and-forget; reentry guarded by currentIdx check
    resetAutoAdvance();
  }

  function next() { goTo(currentIdx + 1); }
  function prev() { goTo(currentIdx - 1); }

  // ─── Auto-advance ─────────────────────────────────────────────────
  let autoTimer: ReturnType<typeof setInterval> | null = null;
  function resetAutoAdvance() {
    if (autoTimer) clearInterval(autoTimer);
    if (autoAdvanceMs > 0 && photos.length > 1) {
      autoTimer = setInterval(next, autoAdvanceMs);
    }
  }

  // ─── Keyboard wiring ──────────────────────────────────────────────
  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'ArrowRight' || e.key === 'PageDown') { next(); e.preventDefault(); }
    else if (e.key === 'ArrowLeft' || e.key === 'PageUp') { prev(); e.preventDefault(); }
  };
  if (bindKeyboard && typeof window !== 'undefined') {
    window.addEventListener('keydown', onKey);
  }

  // Kick off — load first photo + start the auto-advance. Label updates
  // synchronously inside loadCurrent before the async splat-mesh load
  // begins, so the user sees the title immediately.
  void loadCurrent();
  resetAutoAdvance();

  // Self-driving rotation via onBeforeRender. Three.js calls this on every
  // frame the object is rendered, so the splat spins even when consumed
  // through the manifest pipeline (which doesn't drive per-frame ticks).
  // Attached to splatHolder (not the cell `group`) so the label stays put
  // while the splat rotates.
  let lastFrameMs = performance.now();
  splatHolder.onBeforeRender = () => {
    const now = performance.now();
    const dt = (now - lastFrameMs) / 1000;
    lastFrameMs = now;
    if (rotateRadPerSec > 0 && dt > 0 && dt < 1) {
      splatHolder.rotation.y += rotateRadPerSec * dt;
    }
  };

  return {
    group,
    next, prev, goTo,
    currentIndex: () => currentIdx,
    // Public tick() is a no-op — rotation is self-driven via onBeforeRender.
    // Kept on the interface for parity with other LiveCell-shaped modules.
    tick: (_time: number) => {
      lastTickTime = _time;   // suppress unused warning
    },
    dispose: () => {
      disposed = true;
      if (autoTimer) clearInterval(autoTimer);
      if (bindKeyboard && typeof window !== 'undefined') {
        window.removeEventListener('keydown', onKey);
      }
      splatHolder.onBeforeRender = () => {};
      disposeMesh(currentMesh);
      currentMesh = null;
      label.dispose();
    },
  };
}
