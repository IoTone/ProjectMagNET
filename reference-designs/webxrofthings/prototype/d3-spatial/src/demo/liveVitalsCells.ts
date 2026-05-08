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

export interface LiveCell {
  group: THREE.Group;
  tick(time: number): void;
  dispose(): void;
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
}

export function buildLiveLineCell(opts: LiveLineOpts): LiveCell {
  const { url, refreshMs = 30000, width = 0.32, height = 0.16, color = TEXT.primary } = opts;
  const group = new THREE.Group();
  group.name = `live-line:${url}`;

  // Placeholder so the cell isn't empty before the first fetch lands.
  let current: THREE.Group = buildLineMark(
    [new THREE.Vector3(-width / 2, 0, 0), new THREE.Vector3(width / 2, 0, 0)],
    { color: TEXT.muted, radius: 0.002 },
  );
  group.add(current);

  async function refresh() {
    try {
      const resp = await fetch(url);
      if (!resp.ok) return;
      const json = await resp.json() as { samples?: Array<{ t: number; v: number }> } | Array<{ t: number; v: number }>;
      const samples = Array.isArray(json) ? json : (json.samples ?? []);
      if (samples.length < 2) return;

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
    } catch {
      /* leave placeholder / last-good in place */
    }
  }

  refresh();
  const handle = setInterval(refresh, refreshMs);

  return {
    group,
    tick: () => { /* static between refreshes */ },
    dispose: () => { clearInterval(handle); },
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
  } = opts;

  const group = new THREE.Group();
  group.name = `live-targets:${url}`;

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
  group.add(new THREE.Mesh(markerGeo, markerMat));

  /* Pre-allocate target glyphs; reposition per refresh. */
  const glyphGeo = new THREE.SphereGeometry(0.008, 12, 12);
  const glyphMats: THREE.MeshBasicMaterial[] = [];
  const glyphs: THREE.Mesh[] = [];
  for (let i = 0; i < MAX_TARGETS; i++) {
    const mat = new THREE.MeshBasicMaterial({
      color: TARGET_PALETTE[i % TARGET_PALETTE.length]!,
      transparent: true, opacity: 0.95,
    });
    glyphMats.push(mat);
    const m = new THREE.Mesh(glyphGeo, mat);
    m.visible = false;
    group.add(m);
    glyphs.push(m);
  }

  async function refresh() {
    try {
      const resp = await fetch(url);
      if (!resp.ok) return;
      const data = await resp.json() as {
        count?: number;
        targets?: Array<{ id: number; x_m: number; y_m: number }>;
      };
      const targets = data.targets ?? [];

      for (let i = 0; i < MAX_TARGETS; i++) {
        const t = targets[i];
        if (t && Number.isFinite(t.x_m) && Number.isFinite(t.y_m)) {
          const p = mapXY(t.x_m, t.y_m);
          /* Clamp to cell rectangle so a far-out target sticks at the edge
           * rather than disappearing — keeps the radar's view of out-of-cone
           * targets visible. */
          const cx = Math.max(-width / 2, Math.min(width / 2, p.x));
          const cy = Math.max(-height / 2, Math.min(height / 2, p.y));
          glyphs[i]!.position.set(cx, cy, 0.005);
          glyphs[i]!.visible = true;
        } else {
          glyphs[i]!.visible = false;
        }
      }
    } catch {
      for (const g of glyphs) g.visible = false;
    }
  }

  refresh();
  const handle = setInterval(refresh, refreshMs);

  return {
    group,
    tick: () => { /* static between refreshes */ },
    dispose: () => {
      clearInterval(handle);
      coneGeo.dispose();
      hrArcGeo.dispose();
      markerGeo.dispose();
      glyphGeo.dispose();
      lineMat.dispose();
      markerMat.dispose();
      for (const m of glyphMats) m.dispose();
    },
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

  async function refresh() {
    try {
      const resp = await fetch(url);
      if (!resp.ok) return;
      const data = await resp.json() as number[][];
      if (!Array.isArray(data) || data.length < 1 || (data[0]?.length ?? 0) < 2) return;

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
    } catch {
      /* leave previous frame in place */
    }
  }

  refresh();
  const handle = setInterval(refresh, refreshMs);

  return {
    group,
    tick: (time: number) => { currentViz?.tick(time); },
    dispose: () => { clearInterval(handle); },
  };
}
