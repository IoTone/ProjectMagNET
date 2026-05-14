/**
 * Mark builder registrations — bridges manifest schema to viz builders.
 *
 * Each builder takes a MarkSpec and returns a LoadedMark ready for the scene.
 * Call registerAllBuilders() once at startup.
 */

import * as THREE from 'three';
import { registerMarkBuilder, extractHierarchy, extractGraph, extractFlow, extractDistributions, extractSeries } from './loader';
import type { MarkSpec } from './schema';
import type { LoadedMark } from './loader';
import { buildTree } from '../viz/tree';
import { buildTreemap } from '../viz/treemap';
import { buildSunburst } from '../viz/sunburst';
import { buildCircularPack } from '../viz/pack';
import { buildForceGraph } from '../viz/force';
import { buildRidgeline } from '../viz/ridgeline';
import { buildSankey } from '../viz/sankey';
import { buildStreamgraph } from '../viz/streamgraph';
import { buildVideoPanel } from '../viz/videoPanel';
import { buildLiveImuCell } from '../demo/liveImuCell';
import { buildLiveSpatialAudioCell } from '../demo/liveSpatialAudioCell';
import { buildLiveSplatGalleryCell, type SplatPhoto } from '../demo/liveSplatGalleryCell';
import { buildLineMark } from '../chart/marks/line';
import { buildBarMark } from '../chart/marks/bar';
import { buildScatterMark } from '../chart/marks/scatter';
import { buildArcMark } from '../chart/marks/arc';
import { TEXT } from '../ui/palette';

/* Helpers reused by line / bar / scatter / arc adapters. */

interface SeriesPoint { t: number; v: number; }

function disposeGroupTree(g: THREE.Object3D) {
  g.traverse(o => {
    const m = o as THREE.Mesh;
    if ((m as any).isMesh) {
      m.geometry?.dispose?.();
      const mat = m.material as THREE.Material | THREE.Material[] | undefined;
      if (Array.isArray(mat)) mat.forEach(x => x.dispose());
      else mat?.dispose?.();
    }
  });
}

/** Map a `{t, v}[]` series to Vector3 points within a width × height panel
 *  centered on the origin. Returns the points + the auto-fit ranges. */
function seriesToPoints(
  series: SeriesPoint[],
  width: number, height: number,
  vMinFixed?: number, vMaxFixed?: number,
) {
  let tMin = Infinity, tMax = -Infinity, vMin = Infinity, vMax = -Infinity;
  for (const s of series) {
    if (s.t < tMin) tMin = s.t;
    if (s.t > tMax) tMax = s.t;
    if (s.v < vMin) vMin = s.v;
    if (s.v > vMax) vMax = s.v;
  }
  const tSpan = (tMax - tMin) || 1;
  const vRangeMin = vMinFixed ?? vMin;
  const vRangeMax = vMaxFixed ?? vMax;
  const vSpan = (vRangeMax - vRangeMin) || 1;
  const points = series.map(s => new THREE.Vector3(
    ((s.t - tMin) / tSpan) * width  - width / 2,
    ((s.v - vRangeMin) / vSpan) * height - height / 2,
    0,
  ));
  return { points, tMin, tMax, vMin: vRangeMin, vMax: vRangeMax };
}

function makeMark(spec: MarkSpec, group: THREE.Group, viz: unknown, defaults?: Partial<LoadedMark>): LoadedMark {
  return {
    id: spec.id,
    type: spec.type,
    title: spec.title,
    subtitle: spec.subtitle,
    group,
    viz,
    drillable: spec.drillable ?? defaults?.drillable ?? false,
    hoverable: spec.hoverable ?? defaults?.hoverable ?? false,
    draggable: spec.draggable ?? defaults?.draggable ?? false,
  };
}

export function registerAllBuilders() {
  registerMarkBuilder('tree', (spec) => {
    const data = extractHierarchy(spec);
    if (!data) return null;
    const viz = buildTree(data, { form: (spec.config?.form as any) ?? 'radial' });
    return makeMark(spec, viz.group, viz, { drillable: true, hoverable: true });
  });

  registerMarkBuilder('treemap', (spec) => {
    const data = extractHierarchy(spec);
    if (!data) return null;
    const viz = buildTreemap(data);
    return makeMark(spec, viz.group, viz, { drillable: true, hoverable: true });
  });

  registerMarkBuilder('sunburst', (spec) => {
    const data = extractHierarchy(spec);
    if (!data) return null;
    const viz = buildSunburst(data);
    return makeMark(spec, viz.group, viz, { drillable: true, hoverable: true });
  });

  registerMarkBuilder('pack', (spec) => {
    const data = extractHierarchy(spec);
    if (!data) return null;
    const viz = buildCircularPack(data);
    return makeMark(spec, viz.group, viz, { drillable: true, hoverable: true });
  });

  registerMarkBuilder('force', (spec) => {
    const data = extractGraph(spec);
    if (!data) return null;
    const viz = buildForceGraph(data);
    return makeMark(spec, viz.group, viz, { hoverable: true, draggable: true });
  });

  registerMarkBuilder('ridgeline', (spec) => {
    const data = extractDistributions(spec);
    if (!data) return null;
    const viz = buildRidgeline(data);
    return makeMark(spec, viz.group, viz);
  });

  registerMarkBuilder('sankey', (spec) => {
    const data = extractFlow(spec);
    if (!data) return null;
    const viz = buildSankey(data);
    return makeMark(spec, viz.group, viz, { hoverable: true });
  });

  registerMarkBuilder('streamgraph', (spec) => {
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const opts = {
      width: (cfg.width as number) ?? 0.36,
      height: (cfg.height as number) ?? 0.18,
      windowSize: (cfg.windowSize as number) ?? 60,
      scrollSpeed: (cfg.scrollSpeed as number) ?? 8,
    };
    const labelsFor = (dist: number[][]): string[] =>
      (cfg.categories as string[] | undefined) ?? dist.map((_, i) => `series-${i}`);

    const wrapper = new THREE.Group();
    wrapper.name = `streamgraph:${spec.id}`;

    let viz: ReturnType<typeof buildStreamgraph> | null = null;
    function rebuild(dist: number[][]) {
      // Replace the inner streamgraph entirely. Avoids needing setData() on
      // buildStreamgraph and matches what the gallery's live phases cell does.
      if (viz) {
        wrapper.remove(viz.group);
        viz.group.traverse(o => {
          const m = o as THREE.Mesh;
          if ((m as any).isMesh) {
            m.geometry?.dispose?.();
            const mat = m.material as THREE.Material | THREE.Material[] | undefined;
            if (Array.isArray(mat)) mat.forEach(x => x.dispose());
            else mat?.dispose?.();
          }
        });
      }
      const labels = labelsFor(dist);
      const series = dist.map((values, i) => ({ category: labels[i] ?? `series-${i}`, values }));
      viz = buildStreamgraph(series, opts);
      wrapper.add(viz.group);
    }

    const initial = extractDistributions(spec);
    if (initial && initial.length > 0) rebuild(initial);

    const mark = makeMark(spec, wrapper, viz, { hoverable: true });
    mark.refresh = (s) => {
      const dist = extractDistributions(s);
      if (dist && dist.length > 0) rebuild(dist);
      // Keep mark.viz pointing at the live inner viz so callers ticking it work.
      mark.viz = viz;
    };
    return mark;
  });

  // ─── line mark (time-series ribbon) ─────────────────────────────────
  // The chart-style line mark expects THREE.Vector3 points, not raw {t, v}.
  // This adapter normalises the device's series into a width × height panel.
  // Rebuilds in-place on refresh so the same group is reused.
  registerMarkBuilder('line', (spec) => {
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const width  = (cfg.width  as number) ?? 0.32;
    const height = (cfg.height as number) ?? 0.16;
    const color  = (cfg.color  as number) ?? TEXT.primary;
    const vMinFixed = cfg.vMin as number | undefined;
    const vMaxFixed = cfg.vMax as number | undefined;

    const group = new THREE.Group();
    group.name = `line:${spec.id}`;

    let current: THREE.Group = buildLineMark(
      [new THREE.Vector3(-width / 2, 0, 0), new THREE.Vector3(width / 2, 0, 0)],
      { color: TEXT.muted, radius: 0.002 },
    );
    group.add(current);

    function disposeCurrent() {
      group.remove(current);
      current.traverse(o => {
        const m = o as THREE.Mesh;
        if ((m as any).isMesh) {
          m.geometry?.dispose?.();
          const mat = m.material as THREE.Material | THREE.Material[] | undefined;
          if (Array.isArray(mat)) mat.forEach(x => x.dispose());
          else mat?.dispose?.();
        }
      });
    }

    function applySeries(series: Array<{ t: number; v: number }>) {
      if (series.length < 2) return;
      let tMin = Infinity, tMax = -Infinity, vMin = Infinity, vMax = -Infinity;
      for (const s of series) {
        if (s.t < tMin) tMin = s.t;
        if (s.t > tMax) tMax = s.t;
        if (s.v < vMin) vMin = s.v;
        if (s.v > vMax) vMax = s.v;
      }
      const tSpan = (tMax - tMin) || 1;
      const vRangeMin = vMinFixed ?? vMin;
      const vRangeMax = vMaxFixed ?? vMax;
      const vSpan = (vRangeMax - vRangeMin) || 1;
      const points = series.map(s => new THREE.Vector3(
        ((s.t - tMin) / tSpan) * width  - width / 2,
        ((s.v - vRangeMin) / vSpan) * height - height / 2,
        0,
      ));
      const next = buildLineMark(points, { color, radius: 0.003 });
      disposeCurrent();
      group.add(next);
      current = next;
    }

    const initial = extractSeries(spec) as Array<{ t: number; v: number }> | null;
    if (initial && initial.length >= 2) applySeries(initial);

    const mark = makeMark(spec, group, null, { hoverable: spec.hoverable });
    mark.refresh = (s) => {
      const series = extractSeries(s) as Array<{ t: number; v: number }> | null;
      if (series && series.length >= 2) applySeries(series);
    };
    return mark;
  });

  // ─── bar mark — series → bars from baseline ─────────────────────────
  registerMarkBuilder('bar', (spec) => {
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const width  = (cfg.width  as number) ?? 0.32;
    const height = (cfg.height as number) ?? 0.16;
    const color  = (cfg.color  as number) ?? TEXT.warn;
    const barWidth = (cfg.barWidth as number) ?? 0.005;
    const barDepth = (cfg.barDepth as number) ?? 0.005;
    const vMinFixed = cfg.vMin as number | undefined;
    const vMaxFixed = cfg.vMax as number | undefined;

    const group = new THREE.Group();
    group.name = `bar:${spec.id}`;
    let current: THREE.Group | null = null;

    function applySeries(series: SeriesPoint[]) {
      if (series.length === 0) return;
      const { points } = seriesToPoints(series, width, height, vMinFixed, vMaxFixed);
      const next = buildBarMark(points, -height / 2, { color, width: barWidth, depth: barDepth });
      if (current) { group.remove(current); disposeGroupTree(current); }
      group.add(next);
      current = next;
    }

    const initial = extractSeries(spec) as SeriesPoint[] | null;
    if (initial && initial.length > 0) applySeries(initial);

    const mark = makeMark(spec, group, null, { hoverable: spec.hoverable });
    mark.refresh = (s) => {
      const series = extractSeries(s) as SeriesPoint[] | null;
      if (series && series.length > 0) applySeries(series);
    };
    return mark;
  });

  // ─── scatter mark — series → instanced spheres ──────────────────────
  registerMarkBuilder('scatter', (spec) => {
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const width  = (cfg.width  as number) ?? 0.32;
    const height = (cfg.height as number) ?? 0.16;
    const color  = (cfg.color  as number) ?? TEXT.accent;
    const radius = (cfg.radius as number) ?? 0.005;
    const vMinFixed = cfg.vMin as number | undefined;
    const vMaxFixed = cfg.vMax as number | undefined;

    const group = new THREE.Group();
    group.name = `scatter:${spec.id}`;
    let current: THREE.Group | null = null;

    function applySeries(series: SeriesPoint[]) {
      if (series.length === 0) return;
      const { points } = seriesToPoints(series, width, height, vMinFixed, vMaxFixed);
      const next = buildScatterMark(points, { color, radius });
      if (current) { group.remove(current); disposeGroupTree(current); }
      group.add(next);
      current = next;
    }

    const initial = extractSeries(spec) as SeriesPoint[] | null;
    if (initial && initial.length > 0) applySeries(initial);

    const mark = makeMark(spec, group, null, { hoverable: spec.hoverable });
    mark.refresh = (s) => {
      const series = extractSeries(s) as SeriesPoint[] | null;
      if (series && series.length > 0) applySeries(series);
    };
    return mark;
  });

  // ─── arc mark — gauge: latest series sample maps to arc length ──────
  registerMarkBuilder('arc', (spec) => {
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const radius      = (cfg.radius      as number) ?? 0.06;
    const tubeRadius  = (cfg.tubeRadius  as number) ?? 0.003;
    const color       = (cfg.color       as number) ?? TEXT.primary;
    const min         = (cfg.min         as number) ?? 0;
    const max         = (cfg.max         as number) ?? 100;
    const startAngle  = (cfg.startAngle  as number) ?? -Math.PI * 0.75;
    const endFullAngle = (cfg.endAngle   as number) ?? Math.PI * 0.25;
    const span = endFullAngle - startAngle;

    const group = new THREE.Group();
    group.name = `arc:${spec.id}`;
    let current: THREE.Group | null = null;

    function applyValue(value: number) {
      const t = Math.max(0, Math.min(1, (value - min) / ((max - min) || 1)));
      const arcEnd = startAngle + span * t;
      /* When t=0 the arc would be degenerate; nudge to a tiny visible sliver. */
      const drawnEnd = (t < 0.01) ? startAngle + span * 0.01 : arcEnd;
      const next = buildArcMark({ color, radius, tubeRadius, startAngle, endAngle: drawnEnd });
      if (current) { group.remove(current); disposeGroupTree(current); }
      group.add(next);
      current = next;
    }

    /** Read a value to display: latest series sample > config.value > midpoint. */
    function readValue(s: MarkSpec): number {
      const series = extractSeries(s) as SeriesPoint[] | null;
      if (series && series.length > 0) return series[series.length - 1]!.v;
      const cfgVal = (s.config as Record<string, unknown> | undefined)?.value;
      if (typeof cfgVal === 'number') return cfgVal;
      return (min + max) / 2;
    }

    applyValue(readValue(spec));

    const mark = makeMark(spec, group, null, { hoverable: spec.hoverable });
    mark.refresh = (s) => { applyValue(readValue(s)); };
    return mark;
  });

  registerMarkBuilder('video', (spec) => {
    if (spec.data.source !== 'url') return null;
    const url = (spec.data as any).url as string;
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const viz = buildVideoPanel({
      url,
      type: (cfg.type as any) ?? 'hls',
      width: (cfg.width as number) ?? 0.4,
      aspectRatio: (cfg.aspectRatio as number) ?? 16 / 9,
      title: spec.title,
      autoplay: (cfg.autoplay as boolean) ?? true,
      muted: (cfg.muted as boolean) ?? true,
    });
    return makeMark(spec, viz.group, viz);
  });

  // ─── imu mark — UC4 airplane orientation ────────────────────────────
  //
  // Spec shape:
  //   { id, type: 'imu', title, data: { source: 'url', url, shape: 'imu',
  //     refreshInterval: <seconds> }, config: { size, color, smooth } }
  //
  // The cell builds a small wireframe airplane that rotates by the latest
  // IMU snapshot. Today UC4 points this at the mock server's simulated
  // feed; swapping for a real device is a manifest URL change only.
  registerMarkBuilder('imu', (spec) => {
    if (spec.data.source !== 'url') return null;
    const url = (spec.data as any).url as string;
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    // refreshInterval is in seconds in the manifest (matches video/line);
    // convert to ms for the cell. 200 ms (5 Hz) reads as smooth airplane
    // motion via the cell's onBeforeRender slerp.
    const refreshMs = typeof (spec.data as any).refreshInterval === 'number'
      ? Math.max(50, (spec.data as any).refreshInterval * 1000)
      : 200;
    const cell = buildLiveImuCell({
      url,
      refreshMs,
      size:   (cfg.size   as number) ?? 0.10,
      color:  (cfg.color  as number) ?? TEXT.primary,
      smooth: (cfg.smooth as boolean) ?? true,
    });
    return makeMark(spec, cell.group, cell, { hoverable: spec.hoverable });
  });

  // ─── spatial-audio mark — UC4 boombox ────────────────────────────────
  //
  // Spec shape (data is inline-only since the audio is procedural):
  //   { id, type: 'spatial-audio', title, data: { source: 'inline' },
  //     config: { size, bodyColor, accentColor, refDistance, gain, autoplay } }
  //
  // The cell finds the scene's AudioListener via `window.__demo.audioListener`
  // (main.ts exposes it there). If missing — headless tests, early-construction
  // ordering — the visual mesh still renders but no audio plays.
  registerMarkBuilder('spatial-audio', (spec) => {
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const cell = buildLiveSpatialAudioCell({
      size:        (cfg.size        as number)  ?? 0.08,
      bodyColor:   (cfg.bodyColor   as number)  ?? TEXT.muted,
      accentColor: (cfg.accentColor as number)  ?? TEXT.primary,
      refDistance: (cfg.refDistance as number)  ?? 0.6,
      gain:        (cfg.gain        as number)  ?? 0.7,
      autoplay:    (cfg.autoplay    as boolean) ?? true,
    });
    return makeMark(spec, cell.group, cell, { hoverable: spec.hoverable });
  });

  // ─── splat-gallery — UC4 spatial-photo viewer ───────────────────────
  //
  // Spec shape (inline data; photos list lives under config):
  //   { id, type: 'splat-gallery', title,
  //     data: { source: 'inline' },
  //     config: {
  //       photos: [{url, title}, ...],   // 1+ entries, paths served from public/
  //       autoAdvanceMs?: number,        // 0 disables; default 15000
  //       rotateRadPerSec?: number       // 0 disables; default 0.15
  //     }
  //   }
  //
  // Returns null (skipping the mark) if the photo list is empty or missing
  // so a manifest with no asset files doesn't crash the dataspace load.
  registerMarkBuilder('splat-gallery', (spec) => {
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const photos = (cfg.photos as SplatPhoto[] | undefined) ?? [];
    if (!Array.isArray(photos) || photos.length === 0) {
      console.warn(`[splat-gallery] '${spec.id}' has no photos in config; skipping`);
      return null;
    }
    const cell = buildLiveSplatGalleryCell({
      photos,
      autoAdvanceMs:   (cfg.autoAdvanceMs   as number) ?? 15_000,
      rotateRadPerSec: (cfg.rotateRadPerSec as number) ?? 0.15,
      splatY:          (cfg.splatY          as number) ?? 0,
      bindKeyboard:    (cfg.bindKeyboard    as boolean) ?? true,
    });
    return makeMark(spec, cell.group, cell, { hoverable: spec.hoverable });
  });
}
