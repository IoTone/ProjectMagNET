/*
 * owlsToTheMax — a dense grid of stylized cartoon owls procedurally
 * drawn into a canvas and mapped to a horizontal plane on the floor in
 * front of the user. UC3 XRt Exhibit.
 *
 * After Mike Bostock's "Owls" notebook
 * (https://observablehq.com/@mbostock/owls) — the visual joke being a
 * relentlessly dense, identical-yet-not-quite-identical population of
 * the same goofy bird, "to the max."
 *
 * No external assets — every owl is canvas primitives (arcs, fills,
 * triangles). Each owl gets:
 *   - a stable per-cell hue offset (so the carpet isn't monochrome)
 *   - a stable per-cell blink-phase offset (so eyes blink at different
 *     beats and the whole thing doesn't feel mechanical)
 *   - a stable per-cell bob-phase offset (subtle ~0.2 Hz nod)
 *
 * The cell is SELF_POSITIONED: it lays its plane flat on the floor
 * 1.5 m in front of the world origin, just above y=0 so it z-fights
 * with nothing.
 */

import * as THREE from 'three';

export interface OwlsToTheMaxOptions {
  /** Columns × Rows of owls. Default 8 × 6 = 48 birds. */
  cols?: number;
  rows?: number;
  /** Plane size in metres. Default 1.4 × 1.05 in the cell; UC3
   *  manifest authors larger (2.5 × 1.875) to fill the exhibit
   *  ceiling. */
  width?: number;
  height?: number;
  /** Distance in metres in front of the world origin. Default 1.2. */
  distance?: number;
  /** Plane Y in WORLD coords (the cell auto-compensates for parent
   *  anchor offset). When `ceiling=false` this is the floor placement
   *  (default 0.02 — just above the floor); when `ceiling=true` it's
   *  the ceiling placement (default 3.2 — clearly above standing eye). */
  floorY?: number;
  /** When true the plane hangs from above and faces DOWN — a ceiling
   *  tile rather than a floor carpet. Useful when the room has no
   *  visible floor (the UC3 XRt Exhibit doesn't render one) and a
   *  "look up" affordance reads better than "look down." Default false. */
  ceiling?: boolean;
  /** Canvas pixel size (longest side). Higher = sharper at the cost of
   *  per-frame draw time. Default 1024 (gives ~85 px per owl at 8 cols
   *  on a 1.4 m plane — readable from a standing user). */
  pixelSize?: number;
}

export interface OwlsToTheMaxViz {
  group: THREE.Group;
  dispose(): void;
}

/* ─── Spatial owl hoots ─────────────────────────────────────────────
 *
 * Source recording: `public/spatial/268667__depwl9992__owls.mp3` — a
 * field recording of multiple owls hooting over ~30 s. We fetch +
 * decode it once per cell, then on each scheduled play pick a random
 * window of the buffer (1.0–2.5 s long, at a random offset) and play
 * THAT window through a PositionalAudio at a random direction around
 * the user. This gives natural sound (real owls, not a synth) plus
 * variety (different parts of the field recording) plus directionality
 * (random emitter position per hoot).
 *
 * Decode is async + can fail (file missing, fetch error, decode
 * unsupported); all paths null-check the buffer before play. */
const OWL_MP3_URL = '/spatial/268667__depwl9992__owls.mp3';

/** Find the scene's shared AudioListener (set by main.ts on the
 *  global bridge). Returns null in node tests / before audio bootstrap. */
function findSceneListener(): THREE.AudioListener | null {
  if (typeof window === 'undefined') return null;
  const bridge = (window as unknown as { __demo?: { audioListener?: THREE.AudioListener } }).__demo;
  return bridge?.audioListener ?? null;
}

export function buildOwlsToTheMax(opts: OwlsToTheMaxOptions = {}): OwlsToTheMaxViz {
  const {
    cols      = 8,
    rows      = 6,
    width     = 1.4,
    height    = 1.05,
    distance  = 1.2,
    ceiling   = false,
    floorY    = ceiling ? 3.2 : 0.02,
    pixelSize = 1024,
  } = opts;

  const group = new THREE.Group();
  group.name  = 'owls-to-the-max';

  /* ─── Canvas ───────────────────────────────────────────────────── */
  const aspect = width / height;
  const canvasW = pixelSize;
  const canvasH = Math.round(pixelSize / aspect);
  const cnv = document.createElement('canvas');
  cnv.width  = canvasW;
  cnv.height = canvasH;
  /* alpha:true so the background pixels can be transparent — the owls
   * read as floating cut-outs against the sky/scene rather than as a
   * dark-rectangle tile. Pair with MeshBasicMaterial({transparent:true})
   * below so the alpha channel is honoured at the GL layer. */
  const ctx = cnv.getContext('2d', { alpha: true })!;

  /* Per-cell deterministic offsets so each owl is recognisably its own
   * bird. Seeded from cell index so a re-mount produces the same flock. */
  type CellState = { hue: number; blinkPhase: number; bobPhase: number; sizeJitter: number };
  const cells: CellState[] = [];
  for (let i = 0; i < cols * rows; i++) {
    /* Trivial LCG-ish hash for stability without a Random dep. */
    const s = (i * 9301 + 49297) % 233280 / 233280;
    cells.push({
      hue:        (s * 360) % 360,
      blinkPhase: (s * 6.28),
      bobPhase:   ((i * 13.37) % 6.28),
      sizeJitter: 0.88 + (i * 0.137) % 0.18,
    });
  }

  /* Draw ONE owl in a cell of size cellW × cellH at (cx, cy). All
   * coordinates are canvas pixels. */
  function drawOwl(cx: number, cy: number, cellW: number, cellH: number, t: number, state: CellState) {
    const r = Math.min(cellW, cellH) * 0.36 * state.sizeJitter;
    /* Subtle bob — Y-axis offset within ~6% of cell height. */
    const bob = Math.sin(t * 1.2 + state.bobPhase) * cellH * 0.03;
    const y   = cy + bob;

    /* Body — warm brown with hue jitter so the carpet isn't monochrome. */
    ctx.fillStyle = `hsl(${(28 + state.hue * 0.18) % 360}, 45%, ${28 + (state.hue % 12)}%)`;
    ctx.beginPath();
    ctx.ellipse(cx, y, r * 1.05, r * 1.25, 0, 0, Math.PI * 2);
    ctx.fill();

    /* Ear tufts — two pointy triangles. */
    ctx.beginPath();
    ctx.moveTo(cx - r * 0.7, y - r * 1.05);
    ctx.lineTo(cx - r * 0.35, y - r * 1.4);
    ctx.lineTo(cx - r * 0.15, y - r * 0.95);
    ctx.closePath();
    ctx.fill();
    ctx.beginPath();
    ctx.moveTo(cx + r * 0.7, y - r * 1.05);
    ctx.lineTo(cx + r * 0.35, y - r * 1.4);
    ctx.lineTo(cx + r * 0.15, y - r * 0.95);
    ctx.closePath();
    ctx.fill();

    /* Belly — lighter ellipse over the body, off-centre. */
    ctx.fillStyle = `hsl(${(38 + state.hue * 0.18) % 360}, 30%, 78%)`;
    ctx.beginPath();
    ctx.ellipse(cx, y + r * 0.25, r * 0.65, r * 0.85, 0, 0, Math.PI * 2);
    ctx.fill();

    /* Eyes — big disks, blink occasionally. Blink envelope = the inverse
     * of a quick rectified bump so eyes close briefly and then re-open. */
    const blinkBeat = (Math.sin(t * 1.6 + state.blinkPhase) + 1) * 0.5;
    /* Blink threshold: closes when blinkBeat > 0.94 (~3% of the cycle). */
    const blink = blinkBeat > 0.94 ? (blinkBeat - 0.94) / 0.06 : 0;
    const eyeR  = r * 0.32;
    const eyeY  = y - r * 0.15;
    const eyeDx = r * 0.42;

    /* Eye whites */
    ctx.fillStyle = `hsl(${(48 + state.hue * 0.1) % 360}, 85%, ${72 - blink * 50}%)`;
    ctx.beginPath();
    ctx.ellipse(cx - eyeDx, eyeY, eyeR, eyeR * (1 - blink * 0.95), 0, 0, Math.PI * 2);
    ctx.fill();
    ctx.beginPath();
    ctx.ellipse(cx + eyeDx, eyeY, eyeR, eyeR * (1 - blink * 0.95), 0, 0, Math.PI * 2);
    ctx.fill();

    /* Pupils — small dark dots, hide when blinking. */
    if (blink < 0.5) {
      ctx.fillStyle = '#1a1410';
      ctx.beginPath();
      ctx.arc(cx - eyeDx, eyeY, eyeR * 0.45, 0, Math.PI * 2);
      ctx.fill();
      ctx.beginPath();
      ctx.arc(cx + eyeDx, eyeY, eyeR * 0.45, 0, Math.PI * 2);
      ctx.fill();
    }

    /* Beak — tiny upside-down triangle between the eyes. */
    ctx.fillStyle = '#d8a040';
    ctx.beginPath();
    ctx.moveTo(cx,             y + r * 0.05);
    ctx.lineTo(cx - r * 0.08,  y - r * 0.05);
    ctx.lineTo(cx + r * 0.08,  y - r * 0.05);
    ctx.closePath();
    ctx.fill();
  }

  function drawAll(t: number) {
    /* Clear to fully transparent so the canvas alpha channel reads
     * through to the GL plane — the owls hover against the scene
     * sky rather than against a black rectangle. */
    ctx.clearRect(0, 0, canvasW, canvasH);

    const cellW = canvasW / cols;
    const cellH = canvasH / rows;
    for (let row = 0; row < rows; row++) {
      for (let col = 0; col < cols; col++) {
        const idx = row * cols + col;
        const cx = (col + 0.5) * cellW;
        const cy = (row + 0.5) * cellH;
        drawOwl(cx, cy, cellW, cellH, t, cells[idx]!);
      }
    }
  }

  /* Initial render so the first frame has owls. */
  drawAll(0);

  const texture = new THREE.CanvasTexture(cnv);
  texture.minFilter = THREE.LinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.colorSpace = THREE.SRGBColorSpace;

  const planeGeo = new THREE.PlaneGeometry(width, height);
  /* transparent:true is what makes the canvas alpha actually become
   * see-through in the GL pipeline; alphaTest=0.02 trims the soft
   * antialiasing halo around each owl so the cut-outs read cleanly. */
  const planeMat = new THREE.MeshBasicMaterial({
    map: texture,
    transparent: true,
    alphaTest: 0.02,
    depthWrite: false,    // transparent planes shouldn't occlude things behind them
    side: THREE.DoubleSide,
  });
  const mesh = new THREE.Mesh(planeGeo, planeMat);

  /* Floor: rotate -π/2 so the plane's normal points +Y (texture faces up,
   * user looks down). Ceiling: rotate +π/2 so the normal points -Y
   * (texture faces down, user looks up). */
  mesh.rotation.x = ceiling ? Math.PI / 2 : -Math.PI / 2;
  /* Initial Y is just a guess — `floorY` is interpreted as WORLD Y, but
   * the cell ends up attached to vizAnchor (which sits at ~camY-0.20).
   * Without compensation the plane lands at chest height in front of the
   * user, edge-on to forward gaze, and reads as invisible. The
   * onBeforeRender block below adjusts Y once the parent's world matrix
   * is computed (first render), pulling the plane down to true world Y. */
  mesh.position.set(0, floorY, -distance);

  group.add(mesh);

  /* Animate every frame via onBeforeRender — same pattern as the
   * voronoi stippler, gives us free pause-when-not-visible. */
  const startMs = performance.now();
  let anchorCompensated = false;
  const tmpWorld = new THREE.Vector3();
  mesh.onBeforeRender = () => {
    /* One-time Y compensation: the cell may be attached to vizAnchor
     * (≈ camY-0.20 in world Y) rather than world root. floorY is the
     * intended WORLD Y of the plane; subtract the parent's world Y so
     * the plane ends up at floorY regardless of where the anchor sits. */
    if (!anchorCompensated && mesh.parent) {
      mesh.parent.updateMatrixWorld();
      mesh.parent.getWorldPosition(tmpWorld);
      mesh.position.y = floorY - tmpWorld.y;
      anchorCompensated = true;
    }
    const t = (performance.now() - startMs) / 1000;
    drawAll(t);
    texture.needsUpdate = true;
  };

  /* ─── Spatial owl hoots ──────────────────────────────────────────
   *
   * A pool of 4 emitter Object3Ds, each with its own PositionalAudio,
   * is attached to the cell group. Every 2.5–6 s a random emitter is
   * repositioned to a random direction around the user (azimuth + a
   * modest height variation) and triggered.
   *
   * Why a pool: two hoots overlapping from different directions reads
   * far more like a forest than serially-spaced hoots from one rotating
   * source. Pool size 4 leaves headroom for occasional double-hoots.
   *
   * Why position in CELL-local coords (not world coords): the cell is
   * parented to vizAnchor, which itself moves with the dataspace. We
   * want hoots to feel anchored relative to the exhibit, not relative
   * to a fixed world point — if the user re-centres the dataspace, the
   * hoots come along.
   *
   * Listener is grabbed lazily: it's possible (in tests, or pre-audio-
   * init) for window.__demo.audioListener to not yet exist. We poll
   * once per scheduling tick — cheap, and degrades silently if no
   * listener ever shows up. */
  const HOOT_POOL_SIZE = 4;
  /* Hoist disposed BEFORE the hoot closures that read it — they're
   * called async (setTimeout / fetch), so TDZ would never actually
   * trigger, but reading order matters for the next person to scan
   * this file. */
  let disposed = false;
  /* Hoot scheduling state. Allocated even if the listener is missing
   * so dispose() doesn't have to special-case "audio never wired up". */
  let hootListener: THREE.AudioListener | null = null;
  let hootBuffer: AudioBuffer | null = null;
  let hootBufferLoading = false;
  const hootEmitters: THREE.Object3D[] = [];
  const hootAudios:   THREE.PositionalAudio[] = [];
  let hootTimer: ReturnType<typeof setTimeout> | null = null;

  /* Kick the mp3 fetch + decode eagerly so the buffer is (likely)
   * ready by the time the first hoot fires 1.5 s later. We can't
   * decode without an AudioContext, though — so this only starts
   * once the listener exists. ensureHootInfra() handles the lazy
   * listener pickup and chains the decode after it. */
  function loadHootBuffer(ctx: AudioContext): void {
    if (hootBuffer || hootBufferLoading) return;
    hootBufferLoading = true;
    fetch(OWL_MP3_URL)
      .then(r => {
        if (!r.ok) throw new Error(`owls mp3 fetch ${r.status}`);
        return r.arrayBuffer();
      })
      .then(ab => ctx.decodeAudioData(ab))
      .then(buf => {
        if (disposed) return;
        hootBuffer = buf;
        /* Back-fill every already-constructed PositionalAudio with
         * the buffer. ensureHootInfra builds the pool before the
         * buffer arrives, so without this back-fill the first hoots
         * would silently no-op for ~the duration of the fetch. */
        for (const pa of hootAudios) pa.setBuffer(buf);
      })
      .catch(err => {
        console.warn('[owls] hoot mp3 load failed:', err);
      })
      .finally(() => { hootBufferLoading = false; });
  }

  function ensureHootInfra(): boolean {
    if (hootListener) return true;
    const l = findSceneListener();
    if (!l) return false;
    hootListener = l;
    loadHootBuffer(l.context);
    for (let i = 0; i < HOOT_POOL_SIZE; i++) {
      const emitter = new THREE.Object3D();
      emitter.name = `owl-hoot-emitter-${i}`;
      group.add(emitter);
      const pa = new THREE.PositionalAudio(l);
      /* refDistance 2 m: a typical "owl is in a tree maybe 4 m off"
       * feel — closer and it overwhelms when the user happens to face
       * an emitter; farther and the spatialisation gets too subtle to
       * read directionally. */
      pa.setRefDistance(2.0);
      pa.setRolloffFactor(1.0);
      pa.setDistanceModel('inverse');
      pa.setVolume(0.7);
      /* Set buffer up-front if the load already finished, else the
       * .then handler above will back-fill once decoding completes. */
      if (hootBuffer) pa.setBuffer(hootBuffer);
      emitter.add(pa);
      hootEmitters.push(emitter);
      hootAudios.push(pa);
    }
    return true;
  }

  function scheduleNextHoot() {
    if (disposed) return;
    /* 2.0–5.0 s between hoots — real-recording chops are 1.0–2.5 s
     * each, so this keeps the gaps comparable. */
    const delay = 2000 + Math.random() * 3000;
    hootTimer = setTimeout(triggerHoot, delay);
  }

  function triggerHoot() {
    hootTimer = null;
    if (disposed) return;
    if (!ensureHootInfra() || !hootBuffer) {
      /* Either the listener or the decoded buffer isn't ready yet
       * (audio context still pending the first gesture, fetch in
       * flight, etc.). Retry rather than dropping the schedule. */
      scheduleNextHoot();
      return;
    }
    /* Pick a non-playing emitter; fall back to a random one if all
     * are busy (overlapping hoots). */
    let idx = -1;
    for (let i = 0; i < hootAudios.length; i++) {
      if (!hootAudios[i]!.isPlaying) { idx = i; break; }
    }
    if (idx < 0) idx = Math.floor(Math.random() * hootAudios.length);
    const emitter = hootEmitters[idx]!;
    const audio   = hootAudios[idx]!;

    /* Random window into the field recording. The source mp3 is ~30 s
     * of multiple owls hooting; we pick a 1.0–2.5 s slice from a
     * random offset that leaves enough tail for the slice to fit.
     * THREE.Audio passes (offset, duration) through to
     * AudioBufferSourceNode.start, so the player stops automatically
     * at the slice end — no manual setTimeout needed. */
    const bufDur   = hootBuffer.duration;
    const chopDur  = 1.0 + Math.random() * 1.5;
    const maxStart = Math.max(0, bufDur - chopDur);
    const startAt  = Math.random() * maxStart;
    audio.offset   = startAt;
    audio.duration = chopDur;

    /* Position the emitter on a random direction in the user's
     * horizontal plane, radius 3–5 m, slight height variation. The
     * group sits at cell origin; we offset relative to it. The user
     * normally stands at roughly (0, ~1.6, 0) in vizAnchor space, so
     * positions in the (±5, ±1, ±5) box come from all around. */
    const azimuth = Math.random() * Math.PI * 2;
    const radius  = 3 + Math.random() * 2;
    const heightOff = (Math.random() - 0.5) * 1.4;     // ±0.7 m around the plane
    emitter.position.set(
      Math.cos(azimuth) * radius,
      heightOff,
      Math.sin(azimuth) * radius,
    );

    /* Light per-hoot playback-rate variation so the same chop played
     * twice in a row doesn't sound mechanical. ±12% range — beyond
     * that the owls start to sound like ducks / cassette wow. */
    audio.setPlaybackRate(0.88 + Math.random() * 0.24);

    try {
      if (audio.isPlaying) audio.stop();
      audio.play();
    } catch { /* swallow — context may have suspended between checks */ }

    scheduleNextHoot();
  }

  /* Kick the first hoot off after a short warmup so the user has a
   * moment to take in the scene before owls start. */
  hootTimer = setTimeout(triggerHoot, 1500);

  return {
    group,
    dispose() {
      disposed = true;
      mesh.onBeforeRender = () => {};
      texture.dispose();
      planeGeo.dispose();
      planeMat.dispose();
      /* Tear down audio infra — leaving a listener-attached
       * PositionalAudio playing after the cell unloads is exactly the
       * "ghost hoots after leave dataspace" foot-gun. Stop each
       * source, disconnect from the listener graph, drop refs to the
       * buffer so it can be GC'd. */
      if (hootTimer != null) { clearTimeout(hootTimer); hootTimer = null; }
      for (const pa of hootAudios) {
        try { if (pa.isPlaying) pa.stop(); } catch { /* noop */ }
        try { pa.disconnect(); } catch { /* noop */ }
      }
      hootAudios.length = 0;
      for (const e of hootEmitters) { if (e.parent) e.parent.remove(e); }
      hootEmitters.length = 0;
      hootBuffer  = null;
      hootListener = null;
    },
  };
}
