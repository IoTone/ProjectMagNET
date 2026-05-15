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
/** mkkellogg's SplatRenderMode enum values, copied here because the
 *  package's .d.ts doesn't export the enum at the type level even though
 *  the JS does. Dynamic lookup wasn't worth the type acrobatics. */
const SPLAT_RENDER_MODE_TWO_D = 1;   // mkkellogg SplatRenderMode.TwoD
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
  /** Optional location-style subtitle ("Kyoto, Japan"). Rendered in muted
   *  text above the description. */
  location?: string;
  /** Optional multi-line description of the place — shown to the right of
   *  the splat as floating spatial text. Manifest authors can provide
   *  freeform travel-guide copy here. */
  description?: string;
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
  /**
   * Per-scene transform applied to every loaded splat via mkkellogg's
   * `addSplatScene` options. Captures from different photogrammetry tools
   * arrive with wildly different native orientations + scales; the
   * defaults here are tuned so the typical "object-scale capture" sits
   * upright in front of the user. Override in the manifest if your splats
   * land upside-down or off to one side.
   *
   * `position` is in cell-local meters; `scale` is uniform if a single
   * number is provided per axis; `rotation` is a quaternion [x, y, z, w].
   */
  scenePosition?: [number, number, number];
  sceneRotation?: [number, number, number, number];
  sceneScale?: [number, number, number];
}

export interface LiveSplatGalleryCell {
  group: THREE.Group;
  next(): void;
  prev(): void;
  goTo(index: number): void;
  currentIndex(): number;
  tick(time: number): void;
  /**
   * Toggle whether the cell is allowed to do background work — fetching
   * the next compressed .ply, running the auto-advance timer, etc. The
   * cell is constructed in the inactive state and never touches the
   * network until something flips this on (typically applyShowOnly when
   * the user navigates to the photos mode).
   *
   * `setActive(false)` stops the auto-advance timer but keeps the
   * currently-loaded splat in GPU memory — toggling back on is cheap
   * and skips the re-fetch. Use `dispose()` when the cell is being torn
   * down for real.
   */
  setActive(active: boolean): void;
  dispose(): void;
}

/** Module-level instance counter for the splat gallery. The constructor
 *  logs its assigned id, dispose logs the same id. If you see "constructed"
 *  N times with only M < N "disposed", you have leaked cells — exactly the
 *  pre-fix UC4 symptom where Vite HMR accumulated zombie autoTimers. */
let nextCellId = 1;

export function buildLiveSplatGalleryCell(opts: LiveSplatGalleryOpts): LiveSplatGalleryCell {
  const {
    photos,
    autoAdvanceMs = 15_000,
    splatY = 0,
    bindKeyboard = true,
    splatAlphaRemovalThreshold = 5,
    // Defaults rotate 180° around Y so captures whose native camera-frame
    // pointed at -Z face the viewer who looks at -Z in our scene. Scale 1
    // is a starting point; most photogrammetry captures end up roughly
    // 1m-tall after splat-transform's default normalization.
    scenePosition = [0, 0, 0],
    sceneRotation = [0, 1, 0, 0],
    sceneScale    = [1, 1, 1],
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

  // Counter label below the splat ("1 / 2 · DSC 1624").
  const label = new Text();
  label.fontSize = 0.018;
  label.color = TEXT.muted;
  label.anchorX = 'center';
  label.anchorY = 'top';
  label.position.set(0, splatY - 0.25, 0);
  group.add(label);

  // Location heading + description card to the right of the splat. Only
  // visible when the active photo has location/description metadata.
  const locationLabel = new Text();
  locationLabel.fontSize = 0.030;
  locationLabel.color = TEXT.primary;
  locationLabel.anchorX = 'left';
  locationLabel.anchorY = 'top';
  locationLabel.position.set(0.30, splatY + 0.18, 0);
  locationLabel.text = '';
  locationLabel.sync();
  group.add(locationLabel);

  const descriptionLabel = new Text();
  descriptionLabel.fontSize = 0.014;
  descriptionLabel.color = TEXT.body;
  descriptionLabel.anchorX = 'left';
  descriptionLabel.anchorY = 'top';
  descriptionLabel.position.set(0.30, splatY + 0.13, 0);
  descriptionLabel.maxWidth = 0.42;
  descriptionLabel.text = '';
  descriptionLabel.sync();
  group.add(descriptionLabel);

  const cellId = nextCellId++;
  console.info(`[splat-gallery #${cellId}] constructed (${photos.length} photos, autoAdvanceMs=${autoAdvanceMs})`);

  let currentIdx = 0;
  let viewer: DropInViewer | null = null;
  let viewerSceneIndex = -1;   // mkkellogg index of the currently-loaded scene, -1 if none
  let loading = false;
  let disposed = false;
  /** Whether the cell is allowed to do background work. Starts false so the
   *  cell never touches the network or auto-advance until the orchestrator
   *  flips it on (typically applyShowOnly when the photos mode opens). */
  let active = false;

  function updateLabel() {
    const p = photos[currentIdx]!;
    label.text = `${currentIdx + 1} / ${photos.length}  -  ${p.title}`;
    label.sync();
    locationLabel.text = p.location ?? '';
    locationLabel.sync();
    descriptionLabel.text = p.description ?? '';
    descriptionLabel.sync();
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

        // ── Fragment-cost guards (the UC4 Photos perf fixes) ──────────
        //
        // 1. splatRenderMode: TwoD swaps mkkellogg's view-correct 3D
        //    Gaussian ellipsoid for a camera-facing 2D billboard. Cuts
        //    fragment work 2-3× because each splat covers fewer pixels
        //    (the 3D ellipsoid projects to a roughly 1.5-2× larger
        //    screen-space footprint than the equivalent billboard).
        //    Earlier perf passes (alpha threshold 5 → 20 → 35 → 60,
        //    maxScreenSpaceSplatSize 1024 → 256) couldn't cut the
        //    triangle count below ~2.3M because splat-transform already
        //    pre-decimated low-alpha splats at conversion time. With
        //    Path A exhausted, switching the render math is the next
        //    lever before re-converting the source .ply.
        //
        //    Visual trade-off: loses parallax-correct depth — splats
        //    don't change shape as you orbit them. Fine for UC4's
        //    stationary in-flight photo viewer; would be wrong for an
        //    interactive walk-around capture. Flip back to ThreeD (0)
        //    if visual feedback says it's too flat.
        splatRenderMode: SPLAT_RENDER_MODE_TWO_D,

        // 2. maxScreenSpaceSplatSize caps the per-splat fragment footprint.
        //    mkkellogg's default is 1024 — a single foreground splat could
        //    paint a 1024×1024 region (~1M fragments) per frame. Doubly
        //    important now with TwoD billboards since they're flat — a
        //    single sky-plane billboard could cover a huge portion of
        //    the frame.
        maxScreenSpaceSplatSize: 256,

        // NOTE: `freeIntermediateSplatData: true` was tried here and crashed
        // mkkellogg 0.4.7 with "Cannot convert undefined or null to object"
        // at SplatMesh.freeIntermediateSplatData — the buffer it tries to
        // release doesn't exist on every load path (interacts badly with
        // optimizeSplatData / certain render modes). The ~18 MB-per-scene
        // memory saving isn't worth a broken Photos mode. Leave it at
        // default `false` until the upstream library fixes the option.
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
        position: scenePosition,
        rotation: sceneRotation,
        scale:    sceneScale,
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
  //
  // Only runs while `active` is true. `setActive(false)` clears the timer
  // so a hidden cell isn't quietly fetching 6–18 MB compressed .ply files
  // every autoAdvanceMs in the background — that was the dominant UC4 perf
  // hit, since three.js visibility doesn't stop setInterval.
  let autoTimer: ReturnType<typeof setInterval> | null = null;
  function resetAutoAdvance() {
    if (autoTimer) clearInterval(autoTimer);
    autoTimer = null;
    if (!active) return;
    if (autoAdvanceMs > 0 && photos.length > 1) {
      autoTimer = setInterval(next, autoAdvanceMs);
    }
  }

  function setActive(next: boolean) {
    if (next === active) return;
    active = next;
    console.info(`[splat-gallery #${cellId}] setActive(${active})`);
    if (active) {
      // First activation pulls in the first photo. Subsequent reactivations
      // skip the load because viewerSceneIndex is still ≥ 0 from last time.
      if (viewerSceneIndex < 0 && !loading) {
        void loadCurrent();
      }
      resetAutoAdvance();
    } else {
      // Drop the autoTimer but keep the loaded scene in GPU memory so a
      // toggle back to this mode is instant.
      if (autoTimer) clearInterval(autoTimer);
      autoTimer = null;
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

  // Constructor intentionally does NOT kick off loadCurrent() / resetAutoAdvance().
  // The cell sits dormant until applyShowOnly (or the manifest-onLoaded
  // initial-activation loop) calls setActive(true).

  return {
    group,
    next: next, prev, goTo,
    currentIndex: () => currentIdx,
    // Public tick() is a no-op — mkkellogg's DropInViewer self-updates
    // via three's render path. Kept on the interface for parity with
    // other LiveCell-shaped modules.
    tick: (_time: number) => {},
    setActive,
    dispose: () => {
      console.info(`[splat-gallery #${cellId}] disposed`);
      disposed = true;
      active = false;
      if (autoTimer) clearInterval(autoTimer);
      autoTimer = null;
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
      locationLabel.dispose();
      descriptionLabel.dispose();
    },
  };
}
