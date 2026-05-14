/**
 * Live splat gallery cell — UC4 spatial-photo viewer.
 *
 * Renders one Gaussian-splat photo from a list, with next/prev navigation
 * to cycle through. Uses `@mkkellogg/gaussian-splats-3d`'s `DropInViewer`
 * (a `THREE.Group` subclass that owns its own render path) so the
 * manifest pipeline can add it like any other mark with no renderer
 * surgery.
 *
 * Asset note: the file paths in `photos[*].url` must be **compressed PLY**
 * (`.compressed.ply`). The source files in `public/spatial/` are
 * `.sog` (ml-sharp's native output); `npm run convert:spatial` runs them
 * through `@playcanvas/splat-transform` to produce the .ply variants the
 * loader consumes. mkkellogg/gaussian-splats-3d does not load .sog
 * natively, hence the conversion step.
 *
 * UX shape:
 *   - One active splat shown at a time
 *   - Counter text below ("Photo 2 of 3 · DSC 1624")
 *   - Auto-advance every `autoAdvanceMs` (default 15 s)
 *   - Keyboard left/right for manual navigation (window-level listener)
 *
 * Future polish (deferred):
 *   - Click-to-advance arrows in 3D space (requires onSelect wiring in
 *     renderManifest, which currently only handles hover for manifest-built
 *     marks).
 *   - Cross-fade between splats instead of swap-on-load.
 *   - In-XR thumbnail strip rendered from the `.webp` companions in
 *     `public/spatial/`.
 */
import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';

// `@mkkellogg/gaussian-splats-3d` is lazy-loaded the first time a splat
// is needed. Reasons:
//   - The library is sizable; non-UC4 dataspaces shouldn't pay for it.
//   - Module load instantiates worker code that's irrelevant to the
//     manifest builder chain in node tests.
type SplatModule = typeof import('@mkkellogg/gaussian-splats-3d');
type DropInViewerCtor = SplatModule['DropInViewer'];
let splatPromise: Promise<DropInViewerCtor> | null = null;
function loadDropInViewer(): Promise<DropInViewerCtor> {
  if (!splatPromise) {
    splatPromise = import('@mkkellogg/gaussian-splats-3d').then(m => m.DropInViewer);
  }
  return splatPromise;
}
type DropInViewer = InstanceType<DropInViewerCtor>;

export interface SplatPhoto {
  /** Compressed-PLY URL the loader will fetch. Typically `/spatial/foo.compressed.ply`. */
  url: string;
  /** Display label shown under the splat. */
  title: string;
}

export interface LiveSplatGalleryOpts {
  photos: SplatPhoto[];
  /** Auto-advance interval in ms. 0 disables. Default 15000. */
  autoAdvanceMs?: number;
  /** Y position of the splat group relative to the cell origin. Default 0. */
  splatY?: number;
  /** Hookup of keyboard left/right for manual navigation. Default true. */
  bindKeyboard?: boolean;
  /**
   * Splat-alpha removal threshold passed through to mkkellogg's loader.
   * Higher = drop more low-alpha splats = smaller GPU footprint at the
   * cost of some haloing on translucent boundaries. Default 5 (their
   * recommended starting point).
   */
  splatAlphaRemovalThreshold?: number;
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
    splatY = 0,
    bindKeyboard = true,
    splatAlphaRemovalThreshold = 5,
  } = opts;

  if (photos.length === 0) {
    throw new Error('liveSplatGalleryCell: at least one photo required');
  }

  const group = new THREE.Group();
  group.name = 'live-splat-gallery';

  // Holder for the splat viewer + label. The viewer itself is added once
  // (it's expensive to construct) and we swap which splat scene it shows
  // via removeSplatScene / addSplatScene calls.
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
  let viewer: DropInViewer | null = null;
  let viewerSceneIndex = -1;   // mkkellogg index of the currently-loaded scene, -1 if none
  let loading = false;
  let disposed = false;

  function updateLabel() {
    const p = photos[currentIdx]!;
    label.text = `${currentIdx + 1} / ${photos.length}  ·  ${p.title}`;
    label.sync();
  }

  /** Replace the visible splat with the photo at currentIdx. Re-entrant-safe:
   *  if another navigation request arrives while one is in flight, the later
   *  call wins (the in-flight call notices currentIdx changed and yields). */
  async function loadCurrent() {
    if (loading) return;
    loading = true;
    const targetIdx = currentIdx;
    updateLabel();

    let DropInViewerCtor: DropInViewerCtor;
    try {
      DropInViewerCtor = await loadDropInViewer();
    } catch (err) {
      console.error('[splat-gallery] failed to load mkkellogg/gaussian-splats-3d', err);
      loading = false;
      return;
    }
    if (disposed) { loading = false; return; }

    // First-call viewer construction. The viewer is a THREE.Group so we
    // can just add it to our scene-graph anchor.
    if (!viewer) {
      viewer = new DropInViewerCtor({
        // Run mkkellogg's update loop ourselves via the onBeforeRender
        // path it sets up internally — DropInViewer takes care of this
        // when added to a scene that's being rendered.
        sharedMemoryForWorkers: false,   // safer for non-COOP/COEP dev servers
      });
      splatHolder.add(viewer);
    }

    // Remove the currently-loaded scene (if any), then add the new one.
    // mkkellogg's API: indices are sequential, removeSplatScene compacts
    // the list. After remove we always add at the front (index 0).
    if (viewerSceneIndex >= 0) {
      try {
        await (viewer as unknown as { removeSplatScene(i: number, showUI?: boolean): Promise<void> })
          .removeSplatScene(viewerSceneIndex, false);
      } catch (err) {
        console.warn('[splat-gallery] removeSplatScene failed', err);
      }
      viewerSceneIndex = -1;
      if (disposed || currentIdx !== targetIdx) { loading = false; return; }
    }

    const photo = photos[targetIdx]!;
    try {
      await (viewer as unknown as {
        addSplatScene(path: string, opts: Record<string, unknown>): Promise<void>;
      }).addSplatScene(photo.url, {
        splatAlphaRemovalThreshold,
        showLoadingUI: false,
      });
      viewerSceneIndex = 0;
    } catch (err) {
      console.error('[splat-gallery] failed to load', photo.url, err);
    }
    loading = false;

    // If the user navigated during load, queue the next load.
    if (!disposed && currentIdx !== targetIdx) void loadCurrent();
  }

  function goTo(index: number) {
    if (disposed) return;
    const wrapped = ((index % photos.length) + photos.length) % photos.length;
    if (wrapped === currentIdx && viewerSceneIndex >= 0) return;
    currentIdx = wrapped;
    void loadCurrent();
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
  // synchronously inside loadCurrent before the async splat load begins,
  // so the user sees the title immediately even on a cold cache.
  void loadCurrent();
  resetAutoAdvance();

  return {
    group,
    next, prev, goTo,
    currentIndex: () => currentIdx,
    // Public tick() is a no-op — mkkellogg's DropInViewer self-updates
    // via three's render path. Kept on the interface for parity with
    // other LiveCell-shaped modules.
    tick: (_time: number) => {},
    dispose: () => {
      disposed = true;
      if (autoTimer) clearInterval(autoTimer);
      if (bindKeyboard && typeof window !== 'undefined') {
        window.removeEventListener('keydown', onKey);
      }
      if (viewer) {
        const v = viewer as unknown as { dispose?: () => void };
        v.dispose?.();
        splatHolder.remove(viewer);
        viewer = null;
      }
      label.dispose();
    },
  };
}
