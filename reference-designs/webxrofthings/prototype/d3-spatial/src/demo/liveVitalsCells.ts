/**
 * Live vitals gallery cells — fetch real-device data periodically and rebuild
 * the underlying viz. Mirrors how `videoPanel` polls /capture; mirrors how the
 * manifest's `line` / `streamgraph` builders structure their data, but does
 * the polling itself instead of relying on the (one-shot) manifest loader.
 *
 * URLs are relative — they go through the Vite proxy entries declared in
 * vite.config.ts, which strip browser headers and rewrite the prefix to the
 * device's bare endpoints.
 */

import * as THREE from 'three';
import { buildLineMark } from '../chart/marks/line';
import { buildStreamgraph, StreamgraphSeries, StreamgraphViz } from '../viz/streamgraph';
import { TEXT } from '../ui/palette';
import { startPolling, type LiveCellStatus, type LiveCellState } from './livePolling';

// Re-export the polling-layer types so existing consumers
// (`liveStatusBadge`, the gallery wrapper, tests) don't need to know
// about the file split.
export type { LiveCellState, LiveCellStatus } from './livePolling';

export interface LiveCell {
  group: THREE.Group;
  tick(time: number): void;
  dispose(): void;
  /**
   * Connection health derived from the polling loop. Drives the gallery's
   * status badge. Hysteresis rules (in `startPolling`):
   *  - `live`     when no errors and last success is within `2 × refreshMs`
   *               of now (or initial state, before any fetch).
   *  - `stale`    when there's been at least one consecutive error but
   *               the cell isn't yet considered offline.
   *  - `offline`  when there are 3+ consecutive failures and no success
   *               has ever landed, OR when last success is older than
   *               `6 × refreshMs`. Recovery requires 2 consecutive
   *               successes (debounce against single-success flaps).
   */
  getStatus(): LiveCellStatus;
}

function disposeGroup(g: THREE.Object3D) {
  g.traverse((o) => {
    const m = o as THREE.Mesh;
    if ((m as any).isMesh) {
      m.geometry?.dispose?.();
      const mat = m.material as THREE.Material | THREE.Material[] | undefined;
      if (Array.isArray(mat)) mat.forEach(x => x.dispose());
      else mat?.dispose?.();
    }
  });
}

/* ─── Live line — for heart-rate / breathing history ──────────────────── */

export interface LiveLineOpts {
  url: string;
  refreshMs?: number;
  width?: number;
  height?: number;
  color?: number;
  /** Optional fixed Y range; auto-fit per-frame if omitted. */
  vMin?: number;
  vMax?: number;
  /**
   * Scalar / snapshot mode. If set, treats each fetch as a single value
   * (extracted by `pluck`) instead of a series. The cell maintains a
   * rolling client-side buffer of the last `historyLength` plucked values.
   * Use this against snapshot endpoints (`/heart-rate`, `/breathing`)
   * which return the *current* reading rather than a server-side history.
   *
   * Return null/undefined from `pluck` to skip that tick (e.g. when
   * presence is false → BPM is 0 → don't pollute the buffer).
   */
  pluck?: (json: any) => number | null | undefined;
  /** Buffer size for scalar mode. Default 60. */
  historyLength?: number;
}

export function buildLiveLineCell(opts: LiveLineOpts): LiveCell {
  const {
    url, refreshMs = 30000, width = 0.32, height = 0.16, color = TEXT.primary,
    pluck, historyLength = 60,
  } = opts;
  const group = new THREE.Group();
  group.name = `live-line:${url}`;

  // Placeholder so the cell isn't empty before the first fetch lands.
  let current: THREE.Group = buildLineMark(
    [new THREE.Vector3(-width / 2, 0, 0), new THREE.Vector3(width / 2, 0, 0)],
    { color: TEXT.muted, radius: 0.002 },
  );
  group.add(current);

  // Scalar-mode rolling buffer.
  const scalarBuf: Array<{ t: number; v: number }> = [];

  async function refresh(): Promise<boolean> {
    try {
      const resp = await fetch(url);
      if (!resp.ok) return false;
      const json = await resp.json();

      let samples: Array<{ t: number; v: number }>;
      if (pluck) {
        // Snapshot mode — extract one value, push into rolling buffer.
        const v = pluck(json);
        // Pluck returning null is "no presence yet", not a device error.
        if (v == null || !Number.isFinite(v)) return true;
        scalarBuf.push({ t: Date.now(), v });
        while (scalarBuf.length > historyLength) scalarBuf.shift();
        samples = scalarBuf;
      } else {
        // Series mode — accept either bare array or `{samples: [...]}`.
        samples = Array.isArray(json) ? json : (json.samples ?? []);
      }

      if (samples.length < 2) return true;

      let tMin = Infinity, tMax = -Infinity, vMin = Infinity, vMax = -Infinity;
      for (const s of samples) {
        if (s.t < tMin) tMin = s.t;
        if (s.t > tMax) tMax = s.t;
        if (s.v < vMin) vMin = s.v;
        if (s.v > vMax) vMax = s.v;
      }
      const tSpan = (tMax - tMin) || 1;
      const vRangeMin = opts.vMin ?? vMin;
      const vRangeMax = opts.vMax ?? vMax;
      const vSpan = (vRangeMax - vRangeMin) || 1;

      const points = samples.map(s => new THREE.Vector3(
        ((s.t - tMin) / tSpan) * width  - width / 2,
        ((s.v - vRangeMin) / vSpan) * height - height / 2,
        0,
      ));

      const next = buildLineMark(points, { color, radius: 0.003 });
      group.remove(current);
      disposeGroup(current);
      group.add(next);
      current = next;
      return true;
    } catch {
      /* leave placeholder / last-good in place */
      return false;
    }
  }

  const poller = startPolling(refreshMs, refresh);

  return {
    group,
    tick: () => { /* static between refreshes */ },
    dispose: () => { poller.stop(); },
    getStatus: () => poller.getStatus(),
  };
}

/* ─── Live multi-target floor map — for /targets ─────────────────────── */

export interface LiveTargetsOpts {
  url: string;
  refreshMs?: number;
  width?: number;
  height?: number;
  /** Lateral half-extent in metres (cell maps -extent_m..+extent_m to ±width/2). */
  extent_m?: number;
  /** Forward extent in metres (cell maps 0..max_distance_m to bottom..top). */
  max_distance_m?: number;
  /** Half field-of-view in degrees, measured from the +y forward axis. */
  fov_deg?: number;
  /**
   * Tilt in radians around the X axis — visual "lay flat" hint. Defaults to 0
   * (vertical panel like every other cell). Pass e.g. 70°·π/180 to tilt the
   * floor map toward the viewer like a tabletop. Title/subtitle text stays
   * upright because they're attached to the gallery cell, not this group.
   */
  tilt_rad?: number;
  /** Glyph radius in metres. Default 0.012. */
  glyph_radius?: number;
  /** Glyph z-lift above the floor map (looks like the orbs are floating). */
  glyph_lift?: number;
}

const TARGET_PALETTE = [0xff7a8a, 0xffd97a, 0x88ff99]; // warm coral, amber, mint
const MAX_TARGETS = 3;

export function buildLiveTargetsCell(opts: LiveTargetsOpts): LiveCell {
  const {
    url,
    refreshMs = 1000,
    width = 0.32,
    height = 0.16,
    extent_m = 2.5,
    max_distance_m = 4.0,
    fov_deg = 50,
    tilt_rad = 0,
    glyph_radius = 0.012,
    glyph_lift = 0.025,
  } = opts;

  const group = new THREE.Group();
  group.name = `live-targets:${url}`;
  // Tilt around X — leans the floor map toward the viewer like a tabletop.
  // Negative angle pulls the bottom edge (radar marker) closer to the camera.
  if (tilt_rad !== 0) group.rotation.x = -tilt_rad;

  const fovRad = (fov_deg * Math.PI) / 180;
  const radarCellY = -height / 2;
  const tanFov = Math.tan(fovRad);

  /* Map radar (x_m, y_m) → cell-panel coords. */
  const mapXY = (x_m: number, y_m: number) => ({
    x: (x_m / extent_m) * (width / 2),
    y: radarCellY + (y_m / max_distance_m) * height,
  });

  /* Cone outline: two lines from the radar at ±fov, clipped to whichever
   * cell boundary they hit first. */
  let edgeX: number, edgeY: number;
  const fullEdgeLat = height * tanFov;
  if (fullEdgeLat > width / 2) {
    edgeX = width / 2;
    edgeY = (width / 2) / tanFov;
  } else {
    edgeX = fullEdgeLat;
    edgeY = height;
  }
  const coneVerts = new Float32Array([
    0, radarCellY, 0.001,   edgeX, radarCellY + edgeY, 0.001,
    0, radarCellY, 0.001,  -edgeX, radarCellY + edgeY, 0.001,
  ]);
  const coneGeo = new THREE.BufferGeometry();
  coneGeo.setAttribute('position', new THREE.BufferAttribute(coneVerts, 3));
  const lineMat = new THREE.LineBasicMaterial({
    color: TEXT.muted, transparent: true, opacity: 0.55,
  });
  group.add(new THREE.LineSegments(coneGeo, lineMat));

  /* Range arc — 1.5 m, where HR/BR detection is spec'd. */
  function arcGeo(radius_m: number, segments = 32): THREE.BufferGeometry {
    const r = (radius_m / max_distance_m) * height;
    const verts: number[] = [];
    const a0 = Math.PI / 2 - fovRad;
    const a1 = Math.PI / 2 + fovRad;
    for (let i = 0; i <= segments; i++) {
      const t = i / segments;
      const a = a0 + (a1 - a0) * t;
      verts.push(r * Math.cos(a), radarCellY + r * Math.sin(a), 0.001);
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(new Float32Array(verts), 3));
    return g;
  }
  const hrArcGeo = arcGeo(1.5);
  group.add(new THREE.Line(hrArcGeo, lineMat));

  /* Radar marker — small upward triangle at the bottom-center. */
  const markerVerts = new Float32Array([
    -0.005, radarCellY - 0.004, 0.002,
     0.005, radarCellY - 0.004, 0.002,
     0,     radarCellY + 0.005, 0.002,
  ]);
  const markerGeo = new THREE.BufferGeometry();
  markerGeo.setAttribute('position', new THREE.BufferAttribute(markerVerts, 3));
  markerGeo.setIndex([0, 1, 2]);
  const markerMat = new THREE.MeshBasicMaterial({
    color: TEXT.body, transparent: true, opacity: 0.9,
  });
  const markerMesh = new THREE.Mesh(markerGeo, markerMat);
  group.add(markerMesh);

  /* Pre-allocate target glyphs as glowing 3D orbs (Standard material with
   * emissive so the scene's hemisphere + directional light both shade them
   * AND they stay readable against bright passthrough). Repositioned per
   * refresh; scaled to `glyph_radius` so callers can tune. */
  const glyphGeo = new THREE.SphereGeometry(glyph_radius, 18, 14);
  const glyphMats: THREE.MeshStandardMaterial[] = [];
  const glyphs: THREE.Mesh[] = [];
  for (let i = 0; i < MAX_TARGETS; i++) {
    const c = TARGET_PALETTE[i % TARGET_PALETTE.length]!;
    const mat = new THREE.MeshStandardMaterial({
      color: c, emissive: c, emissiveIntensity: 0.7,
      roughness: 0.35, metalness: 0.1,
      transparent: true, opacity: 0.95,
    });
    glyphMats.push(mat);
    const m = new THREE.Mesh(glyphGeo, mat);
    m.visible = false;
    group.add(m);
    glyphs.push(m);
  }

  /* Autonomous-mode state. After consecutive fetch failures totalling
   * AUTONOMOUS_AFTER_OFFLINE_MS, the cell stops trying to render an
   * empty floor map and instead synthesises one or two persons walking
   * through the radar cone. Mirrors the UC4 IMU's autonomous behaviour:
   * the user sees plausible motion even when the device is gone, and
   * the DEMO MODE HUD (driven by the manifest health monitor) tells
   * them the data isn't real. */
  const AUTONOMOUS_AFTER_OFFLINE_MS = 10_000;
  let consecutiveFailMs = 0;
  let lastFailWallMs = 0;
  let autonomousStartMs = 0;

  function applyFakeTargets() {
    /* Two synthetic persons sweeping through the radar cone on slow
     * lissajous-like paths. Periods 17 s / 23 s — mutually prime so the
     * combined motion never visibly repeats. Distances 0.8–2.2 m,
     * lateral ±1.5 m: comfortably inside the BHA2's HR/BR detection
     * cone so a real handover from live → fake → live doesn't visually
     * pop the targets out of the cell. */
    const t = (performance.now() - autonomousStartMs) / 1000;
    const fakes = [
      { x_m: 1.5 * Math.sin(t * (2 * Math.PI / 17)),         y_m: 1.5 + 0.7 * Math.cos(t * (2 * Math.PI / 17)) },
      { x_m: -1.2 * Math.cos(t * (2 * Math.PI / 23) + 0.7),  y_m: 1.2 + 0.6 * Math.sin(t * (2 * Math.PI / 23)) },
    ];
    for (let i = 0; i < MAX_TARGETS; i++) {
      const ft = fakes[i];
      if (ft) {
        const p = mapXY(ft.x_m, ft.y_m);
        const cx = Math.max(-width / 2, Math.min(width / 2, p.x));
        const cy = Math.max(-height / 2, Math.min(height / 2, p.y));
        glyphs[i]!.position.set(cx, cy, glyph_lift);
        glyphs[i]!.visible = true;
      } else {
        glyphs[i]!.visible = false;
      }
    }
  }

  async function refresh(): Promise<boolean> {
    try {
      const resp = await fetch(url);
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const data = await resp.json() as {
        count?: number;
        targets?: Array<{ id: number; x_m: number; y_m: number }>;
      };
      const targets = data.targets ?? [];

      /* Successful fetch — leave autonomous mode if we were in it. */
      consecutiveFailMs = 0;
      autonomousStartMs = 0;

      for (let i = 0; i < MAX_TARGETS; i++) {
        const t = targets[i];
        if (t && Number.isFinite(t.x_m) && Number.isFinite(t.y_m)) {
          const p = mapXY(t.x_m, t.y_m);
          /* Clamp to cell rectangle so a far-out target sticks at the edge
           * rather than disappearing — keeps the radar's view of out-of-cone
           * targets visible. */
          const cx = Math.max(-width / 2, Math.min(width / 2, p.x));
          const cy = Math.max(-height / 2, Math.min(height / 2, p.y));
          glyphs[i]!.position.set(cx, cy, glyph_lift);
          glyphs[i]!.visible = true;
        } else {
          glyphs[i]!.visible = false;
        }
      }
      return true;
    } catch {
      /* Track elapsed offline time using wall clock between failures —
       * `refreshMs` may be inaccurate if the page tab was backgrounded. */
      const now = performance.now();
      if (lastFailWallMs > 0) consecutiveFailMs += now - lastFailWallMs;
      lastFailWallMs = now;

      if (consecutiveFailMs >= AUTONOMOUS_AFTER_OFFLINE_MS) {
        if (autonomousStartMs === 0) autonomousStartMs = now;
        applyFakeTargets();
      } else {
        for (const g of glyphs) g.visible = false;
      }
      return false;
    }
  }

  const poller = startPolling(refreshMs, refresh);

  /* Drive the autonomous-mode animation at render rate. tick() is wired
   * by the demo gallery and the manifest pipeline differently across
   * callsites; onBeforeRender on a permanently-visible child mesh fires
   * reliably in both paths (same pattern as buildLiveImuCell uses for
   * IMU dead-reckoning). No-op when live — synthesised targets only
   * move while consecutive failures keep us in autonomous mode. */
  markerMesh.onBeforeRender = () => {
    if (autonomousStartMs > 0) applyFakeTargets();
  };

  return {
    group,
    tick: () => {
      /* Belt-and-braces: callers that drive tick() get the same
       * animation. onBeforeRender is the primary path. */
      if (autonomousStartMs > 0) applyFakeTargets();
    },
    dispose: () => {
      poller.stop();
      markerMesh.onBeforeRender = () => {};
      coneGeo.dispose();
      hrArcGeo.dispose();
      markerGeo.dispose();
      glyphGeo.dispose();
      lineMat.dispose();
      markerMat.dispose();
      for (const m of glyphMats) m.dispose();
    },
    getStatus: () => poller.getStatus(),
  };
}

/* ─── Live phases streamgraph — for /phases ───────────────────────────── */

export interface LivePhasesOpts {
  url: string;
  refreshMs?: number;
  width?: number;
  height?: number;
  windowSize?: number;
  scrollSpeed?: number;
}

const PHASE_LABELS = ['heart_phase', 'breath_phase', 'total_phase'];

export function buildLivePhasesCell(opts: LivePhasesOpts): LiveCell {
  const { url, refreshMs = 1500, width = 0.32, height = 0.16,
          windowSize = 120, scrollSpeed = 12 } = opts;
  const group = new THREE.Group();
  group.name = `live-phases:${url}`;

  let placeholder: THREE.Object3D | null = buildLineMark(
    [new THREE.Vector3(-width / 2, 0, 0), new THREE.Vector3(width / 2, 0, 0)],
    { color: TEXT.muted, radius: 0.002 },
  );
  group.add(placeholder);

  let currentViz: StreamgraphViz | null = null;

  async function refresh(): Promise<boolean> {
    try {
      const resp = await fetch(url);
      if (!resp.ok) return false;
      const data = await resp.json() as number[][];
      if (!Array.isArray(data) || data.length < 1 || (data[0]?.length ?? 0) < 2) return true;

      const series: StreamgraphSeries[] = data.map((values, i) => ({
        category: PHASE_LABELS[i] ?? `series-${i}`,
        values,
      }));

      if (placeholder) {
        group.remove(placeholder);
        disposeGroup(placeholder);
        placeholder = null;
      }
      if (currentViz) {
        group.remove(currentViz.group);
        disposeGroup(currentViz.group);
      }
      currentViz = buildStreamgraph(series, { width, height, windowSize, scrollSpeed });
      group.add(currentViz.group);
      return true;
    } catch {
      /* leave previous frame in place */
      return false;
    }
  }

  const poller = startPolling(refreshMs, refresh);

  return {
    group,
    tick: (time: number) => { currentViz?.tick(time); },
    dispose: () => { poller.stop(); },
    getStatus: () => poller.getStatus(),
  };
}
