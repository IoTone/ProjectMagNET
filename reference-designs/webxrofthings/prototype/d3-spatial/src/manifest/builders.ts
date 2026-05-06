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
import { buildLineMark } from '../chart/marks/line';
import { TEXT } from '../ui/palette';

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
    const dist = extractDistributions(spec);
    if (!dist) return null;
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const labels = (cfg.categories as string[] | undefined) ?? dist.map((_, i) => `series-${i}`);
    const series = dist.map((values, i) => ({ category: labels[i] ?? `series-${i}`, values }));
    const viz = buildStreamgraph(series, {
      width: (cfg.width as number) ?? 0.36,
      height: (cfg.height as number) ?? 0.18,
      windowSize: (cfg.windowSize as number) ?? 60,
      scrollSpeed: (cfg.scrollSpeed as number) ?? 8,
    });
    return makeMark(spec, viz.group, viz, { hoverable: true });
  });

  // ─── line mark (time-series ribbon) ─────────────────────────────────
  // The chart-style line mark expects THREE.Vector3 points, not raw {t, v}.
  // This adapter normalises the device's series into a width × height panel.
  registerMarkBuilder('line', (spec) => {
    const series = extractSeries(spec) as Array<{ t: number; v: number }> | null;
    const cfg = (spec.config ?? {}) as Record<string, unknown>;
    const width  = (cfg.width  as number) ?? 0.32;
    const height = (cfg.height as number) ?? 0.16;
    const color  = (cfg.color  as number) ?? TEXT.primary;

    const group = new THREE.Group();
    group.name = `line:${spec.id}`;

    // Always emit a group so the mark is visible even before the first fetch
    // populates the series — the renderer can update later via the chart API.
    if (!series || series.length < 2) {
      // Placeholder — a flat line at the baseline so the panel has presence.
      const flat: THREE.Vector3[] = [
        new THREE.Vector3(-width / 2, 0, 0),
        new THREE.Vector3( width / 2, 0, 0),
      ];
      const placeholder = buildLineMark(flat, { color: TEXT.muted, radius: 0.002 });
      group.add(placeholder);
      return makeMark(spec, group, { mode: 'placeholder' }, { hoverable: spec.hoverable });
    }

    let tMin = Infinity, tMax = -Infinity, vMin = Infinity, vMax = -Infinity;
    for (const s of series) {
      if (s.t < tMin) tMin = s.t;
      if (s.t > tMax) tMax = s.t;
      if (s.v < vMin) vMin = s.v;
      if (s.v > vMax) vMax = s.v;
    }
    const tSpan = (tMax - tMin) || 1;
    const vSpan = (vMax - vMin) || 1;

    const points: THREE.Vector3[] = series.map(s => new THREE.Vector3(
      ((s.t - tMin) / tSpan) * width  - width / 2,
      ((s.v - vMin) / vSpan) * height - height / 2,
      0,
    ));

    const line = buildLineMark(points, { color, radius: 0.003 });
    group.add(line);
    return makeMark(spec, group, { points, range: { tMin, tMax, vMin, vMax } },
                    { hoverable: spec.hoverable });
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
}
