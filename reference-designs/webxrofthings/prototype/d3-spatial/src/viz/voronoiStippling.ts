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
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';
import { createGlitchMaterial, type GlitchMaterial } from './glitchTextureShader';

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
  /** When true, arrange three planes into an equilateral triangular
   *  prism (cross-section equilateral triangle, faces 120° apart):
   *    - Face A: the stippled visualisation (existing front).
   *    - Face B: the photograph plain.
   *    - Face C: the photograph with a glitch shader applied.
   *  Mutually exclusive with `mirrorBack` — prism implies the photo is
   *  already on faces B + C. Default false. */
  prism?: boolean;
  /** Caption text rendered at the bottom of each photograph face
   *  (B + C, only when `prism: true`). Face C gets the additional
   *  status line "(status: glitched UofO)" appended. Empty string =
   *  no caption. Default empty. */
  attribution?: string;
  /** Rotation of the whole prism around its central Y (vertical) axis,
   *  in degrees. 0° = face A head-on (the stipple visualisation faces
   *  the camera, the photo + glitch faces are off to the sides).
   *  −60° = the vertical seam between face A (stipple) and face B
   *  (photograph) sits dead-centre to the camera; the viewer sees
   *  equal portions of A + B and face C (glitched) is occluded
   *  behind. This is the curated "vertex-on" gallery composition.
   *  +60° = the C-A edge faces the camera; B is hidden behind.
   *  Default −60°. */
  prismRotationDeg?: number;
  /** Header label rendered at the TOP of each face. Defaults match
   *  the UC3 XRt Exhibit composition; override in the manifest if a
   *  different image needs different titling. Empty string disables
   *  that face's header. */
  prismLabelA?: string;   // default 'Voronoi Stippling'
  prismLabelB?: string;   // default 'Original Photograph'
  prismLabelC?: string;   // default 'Glitched'
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
    mirrorBack       = false,
    prism            = false,
    attribution      = '',
    prismRotationDeg = -60,
    prismLabelA      = 'Voronoi Stippling',
    prismLabelB      = 'Original Photograph',
    prismLabelC      = 'Glitched',
  } = opts;
  /* Silence the unused-import warning that creeps in when TEXT isn't
   * referenced anymore; keep the import in case a future theme call
   * wants it. (Cleaner than ripping it out and re-adding later.) */
  void TEXT;

  const group = new THREE.Group();
  group.name  = 'voronoi-stippling';

  /* Rotate the whole prism around the central Y axis so the viewer
   * lands between a face and a vertical seam by default. Only applied
   * when prism mode is on; non-prism cells stay axis-aligned with the
   * grid like before. The manifest's cell wrapper (with the title +
   * subtitle text) sits OUTSIDE this group, so the caption stays
   * upright regardless of how we spin the prism. */
  if (prism) {
    group.rotation.y = prismRotationDeg * Math.PI / 180;
  }

  /* Triangular-prism geometry. Inscribed circle radius `r` for an
   * equilateral triangle of side `width` is W·√3/6. When `prism` is
   * true face A (the stipple visualisation) gets pushed forward by r
   * so the prism's central axis sits at the cell's local origin;
   * faces B and C then place themselves 120° around that axis. When
   * `prism` is false, faceA remains at the origin and the cell looks
   * exactly like the pre-prism design. */
  const PRISM_R       = prism ? (width * Math.sqrt(3) / 6) : 0;
  const PRISM_SQRT3_2 = Math.sqrt(3) / 2;

  /* Face A wrapper. Even when prism is off, wrapping in a sub-group is
   * a no-op (faceA at origin == direct children of `group`) and keeps
   * the prism / non-prism code paths uniform. */
  const faceA = new THREE.Group();
  faceA.name  = 'voronoi-face-a';
  faceA.position.z = PRISM_R;
  group.add(faceA);

  // Background panel — matches the rest of the manifest cells visually.
  const bgMat = new THREE.MeshBasicMaterial({ color: 0x0f1116, transparent: true, opacity: 0.92 });
  const bg    = new THREE.Mesh(new THREE.PlaneGeometry(width, height), bgMat);
  bg.position.z = -0.001;
  faceA.add(bg);

  // Border
  const edgeMat = new THREE.LineBasicMaterial({ color: 0x9a8a70, transparent: true, opacity: 0.6 });
  const edge    = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.PlaneGeometry(width, height)),
    edgeMat,
  );
  faceA.add(edge);

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
  faceA.add(points);

  /* Face A header label. Sits ABOVE the stipple panel as a child of
   * faceA — rotates with the prism (the manifest mark's own title,
   * rendered by renderManifest in the cell wrapper, stays put in
   * world space and doesn't track the prism rotation). Anchored
   * 'bottom' so the text grows upward from above the panel's top
   * edge. Only added when prism mode is on (non-prism cells already
   * get their title from the manifest layer). */
  let faceAHeader: Text | null = null;
  if (prism && prismLabelA) {
    faceAHeader = new Text();
    faceAHeader.text          = prismLabelA;
    faceAHeader.fontSize      = 0.026;
    faceAHeader.color         = 0xe8e2d4;
    faceAHeader.outlineWidth  = 0.0008;
    faceAHeader.outlineColor  = 0x000000;
    faceAHeader.anchorX       = 'center';
    faceAHeader.anchorY       = 'bottom';
    faceAHeader.maxWidth      = width * 0.92;
    faceAHeader.position.set(0, height / 2 + 0.012, 0.002);
    faceAHeader.sync();
    faceA.add(faceAHeader);
  }

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

  /* Prism-mode state. `prismBuilt` guards against double-construction
   * when img.onload could plausibly fire twice (cache-revalidation
   * paths). `prismRefs` collects each face's geometry / material /
   * caption so dispose() can free them — same shape as backRefs but
   * scaled out to multiple faces. */
  interface PrismFaceRefs {
    group:    THREE.Group;
    mesh:     THREE.Mesh;
    geo:      THREE.PlaneGeometry;
    mat:      THREE.Material;
    tex:      THREE.Texture;
    header?:  Text;
    caption?: Text;
    glitch?:  GlitchMaterial;
  }
  let prismBuilt = false;
  const prismRefs: PrismFaceRefs[] = [];

  /* Build one photograph face. The plain variant uses a vanilla
   * MeshBasicMaterial with the shared texture; the glitch variant
   * swaps in the GLSL effect from `glitchTextureShader`. Caption is
   * troika-three-text anchored to the bottom of the panel; if empty
   * string, no caption mesh is added. */
  function buildPhotoFace(o: {
    width: number; height: number;
    texture: THREE.Texture;
    header: string;
    caption: string;
    glitch: boolean;
  }): PrismFaceRefs {
    const g = new THREE.Group();
    g.name = o.glitch ? 'voronoi-face-c' : 'voronoi-face-b';

    const geo = new THREE.PlaneGeometry(o.width, o.height);
    const glitchMat = o.glitch ? createGlitchMaterial(o.texture) : null;
    const mat = glitchMat ?? new THREE.MeshBasicMaterial({ map: o.texture });
    const mesh = new THREE.Mesh(geo, mat);
    g.add(mesh);

    /* Tick the glitch shader's uTime each frame the mesh would be
     * rendered. Skipped automatically when the cell is out of frustum
     * (onBeforeRender doesn't fire), so a hidden face costs nothing. */
    if (glitchMat) {
      mesh.onBeforeRender = () => {
        if (disposed) return;
        glitchMat.setTime(performance.now() / 1000);
      };
    }

    let headerRef: Text | undefined;
    if (o.header) {
      /* Top-anchored header — matches the face A header so all three
       * face titles sit at the same Y in their local frame. Slightly
       * forward of the plane to avoid z-fighting at grazing angles. */
      const h = new Text();
      h.text         = o.header;
      h.fontSize     = 0.026;
      h.color        = 0xe8e2d4;
      h.outlineWidth = 0.0008;
      h.outlineColor = 0x000000;
      h.anchorX      = 'center';
      h.anchorY      = 'bottom';
      h.maxWidth     = o.width * 0.92;
      h.position.set(0, o.height / 2 + 0.012, 0.002);
      h.sync();
      g.add(h);
      headerRef = h;
    }

    let captionRef: Text | undefined;
    if (o.caption) {
      /* Bottom-anchored caption — sits just outside the panel's lower
       * edge in local space, slightly forward of the plane (z=+0.002)
       * so it doesn't z-fight when the camera is grazing the panel. */
      const t = new Text();
      t.text       = o.caption;
      t.fontSize   = 0.022;
      t.color      = 0xe8e2d4;
      t.outlineWidth = 0.0008;
      t.outlineColor = 0x000000;
      t.anchorX    = 'center';
      t.anchorY    = 'top';
      t.maxWidth   = o.width * 0.92;
      t.position.set(0, -o.height / 2 - 0.012, 0.002);
      t.sync();
      g.add(t);
      captionRef = t;
    }

    return {
      group:   g,
      mesh,
      geo,
      mat,
      tex:     o.texture,
      header:  headerRef,
      caption: captionRef,
      glitch:  glitchMat ?? undefined,
    };
  }

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
     * <img> as the texture source instead of issuing a second fetch.
     * Skipped when `prism` is true — prism builds three faces and the
     * photograph appears on B + C instead. */
    if (mirrorBack && !prism && !backBuilt) {
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

    /* Triangular-prism mode: build faces B (photograph plain) and C
     * (photograph + glitch shader) at 120° / 240° around the prism's
     * central Y axis. Face A (the stippling) is already at the right
     * spot — faceA.position.z = PRISM_R was set at construction.
     *
     * Each face's construction (PlaneGeometry alloc + Texture upload
     * + troika Text SDF rasterisation + glitch ShaderMaterial compile)
     * is non-trivial — collectively ~250 ms on Quest 3, all on one
     * frame's main thread. Defer face B by one rAF and face C by two
     * so the texture upload, the shader compile, and each font-SDF
     * rasterisation land on separate frames; the cell appears to
     * "build itself" over ~50 ms rather than hanging for half a
     * second. The voronoi iteration loop keeps animating in between. */
    if (prism && !prismBuilt) {
      prismBuilt = true;
      const sharedTex = new THREE.Texture(img);
      sharedTex.colorSpace  = THREE.SRGBColorSpace;
      sharedTex.minFilter   = THREE.LinearFilter;
      sharedTex.magFilter   = THREE.LinearFilter;
      sharedTex.needsUpdate = true;

      /* Frame 1: Face B (plain photograph). Position at azimuth 210°
       * on the inscribed circle (face A is at 90°, so B sits 120°
       * clockwise). Rotation aligns the plane's +Z normal with the
       * outward radial direction. */
      requestAnimationFrame(() => {
        if (disposed) return;
        const faceB = buildPhotoFace({
          width, height, texture: sharedTex,
          header:  prismLabelB,
          caption: attribution,
          glitch:  false,
        });
        faceB.group.position.set(-PRISM_R * PRISM_SQRT3_2, 0, -PRISM_R / 2);
        faceB.group.rotation.y = -2 * Math.PI / 3;
        group.add(faceB.group);
        prismRefs.push(faceB);

        /* Frame 2: Face C (photograph + glitch shader). Pulling the
         * glitch ShaderMaterial compile into its own frame is what
         * actually buys most of the load-stutter win on Quest 3. */
        requestAnimationFrame(() => {
          if (disposed) return;
          const captionC = attribution
            ? `${attribution}\n(status: glitched UofO)`
            : '(status: glitched UofO)';
          const faceC = buildPhotoFace({
            width, height, texture: sharedTex,
            header:  prismLabelC,
            caption: captionC,
            glitch:  true,
          });
          faceC.group.position.set(PRISM_R * PRISM_SQRT3_2, 0, -PRISM_R / 2);
          faceC.group.rotation.y = 2 * Math.PI / 3;
          group.add(faceC.group);
          prismRefs.push(faceC);
        });
      });
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
      /* Prism faces share one Texture across B + C — dispose it once
       * via the first face's `tex` ref, then dispose the rest's
       * geometries + materials + caption + header Text widgets. */
      if (faceAHeader) faceAHeader.dispose();
      let texDisposed = false;
      for (const face of prismRefs) {
        face.mesh.onBeforeRender = () => {};
        face.geo.dispose();
        face.mat.dispose();
        if (face.header)  face.header.dispose();
        if (face.caption) face.caption.dispose();
        if (!texDisposed) {
          face.tex.dispose();
          texDisposed = true;
        }
      }
      prismRefs.length = 0;
    },
  };
}
