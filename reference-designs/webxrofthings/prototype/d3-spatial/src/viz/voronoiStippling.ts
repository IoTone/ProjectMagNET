/*
 * voronoiStippling — Bostock's weighted-Voronoi stippling rendered as a
 * flat panel in 3D space.
 *
 * Reference: https://observablehq.com/@mbostock/voronoi-stippling
 *
 * Algorithm (per iteration):
 *   1. Build a Delaunay/Voronoi diagram over the current N stipple
 *      positions.
 *   2. For each Voronoi cell, integrate over the source-image pixels it
 *      contains. Each pixel votes with weight (1 - luminance) — dark
 *      pixels pull stipples toward themselves.
 *   3. Move each stipple to its cell's weighted centroid.
 *   4. Repeat. The pattern converges to a stippled rendering of the
 *      image in ~80–200 iterations.
 *
 * Implementation notes:
 *   - We don't run all iterations up-front (would block the main thread
 *     for several seconds on a 6–10k-point cloud). Instead we run a
 *     small slice (`stepsPerFrame`) inside an animation tick driven by
 *     mesh.onBeforeRender, so the pattern is visibly "developing" the
 *     first second or two and then settles.
 *   - Points are rendered with THREE.Points (one vertex per stipple) at
 *     a small fixed pixel size. Cheaper than InstancedMesh and reads
 *     correctly as ink dots from any distance.
 *   - The source image is loaded into an offscreen canvas; we read the
 *     luminance grid once at startup and reuse it across iterations.
 */

import * as THREE from 'three';
import { Delaunay } from 'd3-delaunay';
import { TEXT } from '../ui/palette';

export interface VoronoiStipplingOptions {
  /** URL of the source image (any same-origin / CORS-permitting raster). */
  imageUrl: string;
  /** Number of stipples. Default 4000. 8000 is dense; >12000 gets slow. */
  samples?: number;
  /** Panel width in world units. Default 0.7. */
  width?: number;
  /** Panel height in world units. Default 0.9 (Voronoi adapts to image aspect). */
  height?: number;
  /** Voronoi iterations per animation frame. Default 1. Higher = converges
   *  faster but more cost per frame. */
  stepsPerFrame?: number;
  /** Maximum iterations. The cell stops iterating once reached. Default 120. */
  maxSteps?: number;
  /** Dot size in pixels (THREE.Points size). Default 1.6. */
  dotSize?: number;
  /** Dot colour (24-bit RGB). Default 0xFFFFFF (white) so stipples pop
   *  against a dark panel background. */
  color?: number;
  /** Optional caption shown above the panel. */
  title?: string;
  /** When true (default), light pixels get more stipples — i.e. a portrait's
   *  face renders dense, the background sparse. This is the visual
   *  inversion of Bostock's original (dark pixels = dense), and reads
   *  much better with white stipples on a dark panel: the subject
   *  silhouettes in bright white.
   *  Set false for the canonical "ink-on-paper" Bostock interpretation. */
  invert?: boolean;
  /** Cap on the longest side of the downsampled luminance grid the
   *  iterator walks per step. Without this, a 1920 × 1280 source image
   *  causes a ~2.5M-pixel walk per iteration × 120 iterations and the
   *  main thread sits at 8 FPS for ~10 seconds while it converges.
   *  256 keeps iterations under ~5 ms each. Stipples are still rendered
   *  at full panel resolution; only the per-iteration weight grid is
   *  downsampled — the visual output is indistinguishable. Default 256. */
  maxGridDim?: number;
  /** When true, add a second plane behind the stippling, rotated 180°
   *  so it faces the opposite direction, showing the source image plain
   *  (no stippling). Useful in an exhibit-walk-around context: stippled
   *  art on one side, the raw photograph on the other. Default false. */
  mirrorBack?: boolean;
}

export interface VoronoiStipplingViz {
  group: THREE.Group;
  /** Current iteration count (read-only). */
  iterations(): number;
  /** Stop the animation timer + free resources. */
  dispose(): void;
}

export function buildVoronoiStippling(opts: VoronoiStipplingOptions): VoronoiStipplingViz {
  const {
    imageUrl,
    samples       = 4000,
    width         = 0.7,
    height        = 0.9,
    stepsPerFrame = 1,
    maxSteps      = 120,
    dotSize       = 1.6,
    color         = 0xFFFFFF,
    title,
    invert        = true,
    maxGridDim    = 256,
    mirrorBack    = false,
  } = opts;
  /* Silence the unused-import warning that creeps in when TEXT isn't
   * referenced anymore; keep the import in case a future theme call
   * wants it. (Cleaner than ripping it out and re-adding later.) */
  void TEXT;

  const group = new THREE.Group();
  group.name  = 'voronoi-stippling';

  // Background panel — matches the rest of the manifest cells visually.
  const bgMat = new THREE.MeshBasicMaterial({ color: 0x0f1116, transparent: true, opacity: 0.92 });
  const bg    = new THREE.Mesh(new THREE.PlaneGeometry(width, height), bgMat);
  bg.position.z = -0.001;
  group.add(bg);

  // Border
  const edgeMat = new THREE.LineBasicMaterial({ color: 0x9a8a70, transparent: true, opacity: 0.6 });
  const edge    = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.PlaneGeometry(width, height)),
    edgeMat,
  );
  group.add(edge);

  if (title) {
    // troika-three-text would add asset dependency; use a plain plane label
    // placeholder if the caller wants something more, they can compose
    // outside. For now the manifest's mark title (rendered by renderManifest)
    // covers the label.
  }

  // Points geometry — we allocate the full attribute up-front so that
  // updating positions every iteration is just a Float32Array write.
  const positions = new Float32Array(samples * 3);
  const geometry  = new THREE.BufferGeometry();
  geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  const pointMat = new THREE.PointsMaterial({ color, size: dotSize, sizeAttenuation: false });
  const points   = new THREE.Points(geometry, pointMat);
  group.add(points);

  // Per-stipple state in image-space coordinates (0..imageW, 0..imageH).
  const stipplesX = new Float32Array(samples);
  const stipplesY = new Float32Array(samples);

  // Image data — populated asynchronously. Until it loads we render the
  // panel with no stipples (just the background frame), then kick off
  // the iteration loop once luminance is in hand.
  let imageW = 0, imageH = 0;
  let weight: Float32Array | null = null;   // 1 - luminance, length = imageW*imageH
  let iter = 0;
  let disposed = false;

  /* Mirror-back state. Declared at builder scope so both `img.onload`
   * (which constructs it lazily once the source image is decoded) and
   * `dispose()` (which frees it) close over the same refs. */
  let backBuilt = false;
  let backRefs: { mesh: THREE.Mesh; geo: THREE.PlaneGeometry; mat: THREE.MeshBasicMaterial; tex: THREE.Texture } | null = null;

  const img = new Image();
  img.crossOrigin = 'anonymous';
  img.onload = () => {
    if (disposed) return;
    /* Downsample the source so the per-iteration pixel walk is bounded.
     * Original implementation iterated at native resolution — a 1920×1280
     * portrait = ~2.5M pixels × ~120 iterations = ~300M pixel-visits,
     * which pegs the main thread at 8 FPS for ~10 seconds. Capping the
     * longest side at maxGridDim (256 by default) keeps each iteration
     * under ~5 ms; visual output is indistinguishable because the
     * stipple count (a few thousand) is orders of magnitude smaller than
     * the pixel grid anyway. */
    const srcW = img.naturalWidth, srcH = img.naturalHeight;
    const scale = Math.min(1, maxGridDim / Math.max(srcW, srcH));
    imageW = Math.max(1, Math.round(srcW * scale));
    imageH = Math.max(1, Math.round(srcH * scale));
    const cnv = document.createElement('canvas');
    cnv.width  = imageW;
    cnv.height = imageH;
    const ctx = cnv.getContext('2d', { willReadFrequently: true });
    if (!ctx) return;
    ctx.drawImage(img, 0, 0, imageW, imageH);
    const data = ctx.getImageData(0, 0, imageW, imageH).data;
    weight = new Float32Array(imageW * imageH);
    for (let i = 0; i < imageW * imageH; i++) {
      const r = data[i * 4]!, g = data[i * 4 + 1]!, b = data[i * 4 + 2]!;
      // Rec. 709 luminance.
      const lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
      /* invert=true  → light pixels weigh more (a portrait's face renders
       *               dense white on the dark panel — what the viewer
       *               wants when stipples are white).
       * invert=false → Bostock's canonical "dark = ink" interpretation. */
      weight[i] = invert ? lum : 1 - lum;
    }
    // Seed stipples via rejection sampling — proportionally denser where
    // the image is darker. Faster to converge than uniform-random seeding.
    let placed = 0;
    let guard  = 0;
    while (placed < samples && guard < samples * 50) {
      const x = Math.random() * imageW;
      const y = Math.random() * imageH;
      const w = weight[(y | 0) * imageW + (x | 0)] ?? 0;
      if (Math.random() < w) {
        stipplesX[placed] = x;
        stipplesY[placed] = y;
        placed++;
      }
      guard++;
    }
    // If the image is overwhelmingly bright, fall back to uniform fill so
    // the panel doesn't look empty.
    for (let i = placed; i < samples; i++) {
      stipplesX[i] = Math.random() * imageW;
      stipplesY[i] = Math.random() * imageH;
    }
    writePointsToBuffer();

    /* Mirror-back: a second plane behind the stippling, facing the
     * opposite direction, showing the raw photograph. Built lazily here
     * (rather than at top-level) so we can reuse the already-loaded
     * <img> as the texture source instead of issuing a second fetch. */
    if (mirrorBack && !backBuilt) {
      backBuilt = true;
      const backTex = new THREE.Texture(img);
      backTex.colorSpace = THREE.SRGBColorSpace;
      backTex.minFilter = THREE.LinearFilter;
      backTex.magFilter = THREE.LinearFilter;
      backTex.needsUpdate = true;
      const backGeo = new THREE.PlaneGeometry(width, height);
      const backMat = new THREE.MeshBasicMaterial({ map: backTex });
      const backMesh = new THREE.Mesh(backGeo, backMat);
      /* Sit just behind the dark bg (z=-0.001). Rotate 180° around Y
       * so the +Z normal becomes -Z (faces opposite). scale.x=-1
       * undoes the texture-mirror that the rotation introduces, so the
       * back image reads correctly oriented from behind. */
      backMesh.position.z = -0.003;
      backMesh.rotation.y = Math.PI;
      backMesh.scale.x    = -1;
      group.add(backMesh);
      backRefs = { mesh: backMesh, geo: backGeo, mat: backMat, tex: backTex };
    }
  };
  img.onerror = () => { /* leave panel empty */ };
  img.src = imageUrl;

  /** Map image-space stipple coords → panel local coords, write into the
   *  Three.js BufferAttribute. Mirrors Y so the image isn't upside-down. */
  function writePointsToBuffer() {
    if (!imageW || !imageH) return;
    for (let i = 0; i < samples; i++) {
      const u = stipplesX[i]! / imageW;
      const v = stipplesY[i]! / imageH;
      positions[i * 3]     = (u - 0.5) * width;
      positions[i * 3 + 1] = (0.5 - v) * height;
      positions[i * 3 + 2] = 0;
    }
    (geometry.attributes.position as THREE.BufferAttribute).needsUpdate = true;
  }

  /** One Lloyd-relaxation iteration weighted by image luminance. */
  function step() {
    if (!weight || !imageW || !imageH) return;
    // Pack stipples into a single Float64Array for Delaunay (it expects
    // interleaved x,y pairs).
    const flat = new Float64Array(samples * 2);
    for (let i = 0; i < samples; i++) {
      flat[i * 2]     = stipplesX[i]!;
      flat[i * 2 + 1] = stipplesY[i]!;
    }
    const delaunay = new Delaunay(flat);

    // Accumulate weighted centroids per cell.
    const sumX = new Float32Array(samples);
    const sumY = new Float32Array(samples);
    const sumW = new Float32Array(samples);

    /* Walk every pixel. `delaunay.find(x, y, hint)` returns the index of
     * the nearest stipple — i.e., which Voronoi cell the pixel belongs to.
     * We use a row-major hint to avoid the O(N) cold-start cost on every
     * pixel. This is the same trick the Observable notebook uses. */
    let hint = 0;
    for (let y = 0; y < imageH; y++) {
      for (let x = 0; x < imageW; x++) {
        const w = weight[y * imageW + x]!;
        if (w <= 0) continue;
        hint = delaunay.find(x + 0.5, y + 0.5, hint);
        sumX[hint]! += (x + 0.5) * w;
        sumY[hint]! += (y + 0.5) * w;
        sumW[hint]! += w;
      }
    }
    // Move each stipple to its cell's weighted centroid; cells that
    // collected no weight (rare — only when a stipple has no image pixels
    // in its Voronoi cell) jitter a tiny bit so they can re-enter the run.
    for (let i = 0; i < samples; i++) {
      const w = sumW[i]!;
      if (w > 0) {
        stipplesX[i] = sumX[i]! / w;
        stipplesY[i] = sumY[i]! / w;
      } else {
        stipplesX[i] = (stipplesX[i]! + (Math.random() - 0.5) * 2) % imageW;
        stipplesY[i] = (stipplesY[i]! + (Math.random() - 0.5) * 2) % imageH;
      }
    }
    writePointsToBuffer();
    iter++;
  }

  // Drive iterations from the panel's onBeforeRender so we get free
  // throttling whenever the cell isn't being rendered. Cheap early-out
  // once we've hit maxSteps so settled stipples don't keep churning CPU.
  points.onBeforeRender = () => {
    if (disposed) return;
    if (iter >= maxSteps) return;
    if (!weight) return;
    for (let k = 0; k < stepsPerFrame; k++) {
      if (iter >= maxSteps) break;
      step();
    }
  };

  return {
    group,
    iterations: () => iter,
    dispose() {
      disposed = true;
      points.onBeforeRender = () => {};
      geometry.dispose();
      pointMat.dispose();
      bgMat.dispose();
      edgeMat.dispose();
      (edge.geometry as THREE.BufferGeometry).dispose();
      if (backRefs) {
        backRefs.mat.dispose();
        backRefs.geo.dispose();
        backRefs.tex.dispose();
      }
    },
  };
}
