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
