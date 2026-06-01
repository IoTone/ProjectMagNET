import { describe, it, expect, beforeAll } from 'vitest';
import * as THREE from 'three';
import { loadManifest } from './loader';
import { registerAllBuilders } from './builders';
import type { DataspaceManifest, MarkSpec } from './schema';

beforeAll(() => {
  registerAllBuilders();
});

/* ─── helpers ───────────────────────────────────────────────────────── */

function ds(marks: MarkSpec[]): DataspaceManifest {
  return {
    version: '1',
    name: 'builders.test',
    scaleTag: 'room',
    marks,
  };
}

const sampleSeries = [
  { t: 1000, v: 60 },
  { t: 2000, v: 70 },
  { t: 3000, v: 65 },
  { t: 4000, v: 80 },
  { t: 5000, v: 75 },
];

const sampleDistributions: number[][] = [
  [10, 12, 14, 16, 14],
  [ 8,  9, 11, 13, 11],
  [ 5,  6,  7,  6,  5],
];

/* All builders that swap inner content on refresh wrap that content in a single
 * child Group. This helper grabs the inner-content uuid so we can assert it
 * actually changed across a refresh. */
function innerUuid(group: THREE.Group): string | null {
  const child = group.children.find(c => (c as THREE.Group).isGroup) as THREE.Group | undefined;
  return child?.uuid ?? null;
}

function meshCount(root: THREE.Object3D): number {
  let n = 0;
  root.traverse(o => { if ((o as THREE.Mesh).isMesh) n++; });
  return n;
}

/* ─── line ──────────────────────────────────────────────────────────── */

describe('line builder', () => {
  it('renders a tube from a valid series', async () => {
    const result = await loadManifest(ds([{
      id: 'hr', type: 'line', title: 'HR',
      data: { source: 'inline', series: sampleSeries },
    } as MarkSpec]));
    expect(result.marks).toHaveLength(1);
    const mark = result.marks[0]!;
    expect(mark.group.name).toBe('line:hr');
    expect(meshCount(mark.group)).toBeGreaterThan(0);
    expect(typeof mark.refresh).toBe('function');
    result.dispose();
  });

  it('shows a muted placeholder when series is empty', async () => {
    const result = await loadManifest(ds([{
      id: 'hr', type: 'line', title: 'HR',
      data: { source: 'inline', series: [] },
    } as MarkSpec]));
    const mark = result.marks[0]!;
    // Placeholder is still a line group inside the wrapper, but built from
    // the synthetic flat segment, not real data.
    expect(meshCount(mark.group)).toBeGreaterThan(0);
    result.dispose();
  });

  it('refresh() swaps the inner line group', async () => {
    const spec = {
      id: 'hr', type: 'line' as const, title: 'HR',
      data: { source: 'inline' as const, series: sampleSeries },
    } as MarkSpec;
    const result = await loadManifest(ds([spec]));
    const mark = result.marks[0]!;
    const before = innerUuid(mark.group);
    expect(before).not.toBeNull();

    // Mutate spec with a new series, fire refresh
    (spec.data as any).series = [
      { t: 6000, v: 90 }, { t: 7000, v: 95 }, { t: 8000, v: 100 },
    ];
    mark.refresh!(spec);

    const after = innerUuid(mark.group);
    expect(after).not.toBeNull();
    expect(after).not.toBe(before);
    result.dispose();
  });

  it('honors config.vMin / vMax fixed scale', async () => {
    const result = await loadManifest(ds([{
      id: 'hr', type: 'line', title: 'HR',
      data: { source: 'inline', series: sampleSeries },
      config: { vMin: 40, vMax: 130 },
    } as MarkSpec]));
    expect(result.marks).toHaveLength(1);
    expect(meshCount(result.marks[0]!.group)).toBeGreaterThan(0);
    result.dispose();
  });
});

/* ─── bar ───────────────────────────────────────────────────────────── */

describe('bar builder', () => {
  it('builds InstancedMesh-backed bars from a series', async () => {
    const result = await loadManifest(ds([{
      id: 'temp', type: 'bar', title: 'Temp',
      data: { source: 'inline', series: sampleSeries },
    } as MarkSpec]));
    const mark = result.marks[0]!;
    expect(mark.group.name).toBe('bar:temp');
    expect(meshCount(mark.group)).toBeGreaterThan(0);
    expect(typeof mark.refresh).toBe('function');
    result.dispose();
  });

  it('refresh() swaps the inner bar group', async () => {
    const spec = {
      id: 'temp', type: 'bar' as const, title: 'Temp',
      data: { source: 'inline' as const, series: sampleSeries },
    } as MarkSpec;
    const result = await loadManifest(ds([spec]));
    const mark = result.marks[0]!;
    const before = innerUuid(mark.group);

    (spec.data as any).series = [{ t: 1000, v: 100 }, { t: 2000, v: 50 }];
    mark.refresh!(spec);

    expect(innerUuid(mark.group)).not.toBe(before);
    result.dispose();
  });
});

/* ─── scatter ───────────────────────────────────────────────────────── */

describe('scatter builder', () => {
  it('builds an instanced scatter from a series', async () => {
    const result = await loadManifest(ds([{
      id: 'pts', type: 'scatter', title: 'Points',
      data: { source: 'inline', series: sampleSeries },
    } as MarkSpec]));
    const mark = result.marks[0]!;
    expect(mark.group.name).toBe('scatter:pts');
    expect(meshCount(mark.group)).toBeGreaterThan(0);
    result.dispose();
  });

  it('refresh() swaps the inner scatter group', async () => {
    const spec = {
      id: 'pts', type: 'scatter' as const, title: 'Points',
      data: { source: 'inline' as const, series: sampleSeries },
    } as MarkSpec;
    const result = await loadManifest(ds([spec]));
    const mark = result.marks[0]!;
    const before = innerUuid(mark.group);

    (spec.data as any).series = [{ t: 100, v: 1 }, { t: 200, v: 2 }, { t: 300, v: 3 }];
    mark.refresh!(spec);

    expect(innerUuid(mark.group)).not.toBe(before);
    result.dispose();
  });
});

/* ─── arc (gauge) ───────────────────────────────────────────────────── */

describe('arc builder', () => {
  it('builds a gauge arc from config.value', async () => {
    const result = await loadManifest(ds([{
      id: 'gauge', type: 'arc', title: 'HR gauge',
      data: { source: 'inline' },
      config: { value: 72, min: 40, max: 130 },
    } as MarkSpec]));
    const mark = result.marks[0]!;
    expect(mark.group.name).toBe('arc:gauge');
    expect(meshCount(mark.group)).toBeGreaterThan(0);
    expect(typeof mark.refresh).toBe('function');
    result.dispose();
  });

  it('uses last sample of an inline series when no config.value', async () => {
    const result = await loadManifest(ds([{
      id: 'gauge', type: 'arc', title: 'HR gauge',
      data: { source: 'inline', series: sampleSeries },
      config: { min: 40, max: 130 },
    } as MarkSpec]));
    const mark = result.marks[0]!;
    expect(meshCount(mark.group)).toBeGreaterThan(0);
    result.dispose();
  });

  it('renders a placeholder when given no value source', async () => {
    const result = await loadManifest(ds([{
      id: 'gauge', type: 'arc', title: 'HR gauge',
      data: { source: 'inline' },
      config: { min: 0, max: 100 },
    } as MarkSpec]));
    // Placeholder lives at midpoint; should still produce mesh.
    expect(meshCount(result.marks[0]!.group)).toBeGreaterThan(0);
    result.dispose();
  });

  it('refresh() rebuilds when the series tail changes', async () => {
    const spec = {
      id: 'gauge', type: 'arc' as const, title: 'HR gauge',
      data: { source: 'inline' as const, series: [{ t: 1, v: 60 }] },
      config: { min: 40, max: 130 },
    } as MarkSpec;
    const result = await loadManifest(ds([spec]));
    const mark = result.marks[0]!;
    const before = innerUuid(mark.group);

    (spec.data as any).series = [{ t: 2, v: 60 }, { t: 3, v: 110 }];
    mark.refresh!(spec);

    expect(innerUuid(mark.group)).not.toBe(before);
    result.dispose();
  });
});

/* ─── streamgraph ────────────────────────────────────────────────────── */

describe('streamgraph builder', () => {
  it('builds layer meshes from inline distributions', async () => {
    const result = await loadManifest(ds([{
      id: 'phases', type: 'streamgraph', title: 'Phases',
      data: { source: 'inline', distributions: sampleDistributions },
      config: { categories: ['heart', 'breath', 'total'] },
    } as MarkSpec]));
    const mark = result.marks[0]!;
    expect(mark.group.name).toBe('streamgraph:phases');
    // One mesh per layer = 3
    expect(meshCount(mark.group)).toBe(3);
    expect(typeof mark.refresh).toBe('function');
    result.dispose();
  });

  it('refresh() swaps the inner streamgraph group', async () => {
    const spec = {
      id: 'phases', type: 'streamgraph' as const, title: 'Phases',
      data: { source: 'inline' as const, distributions: sampleDistributions },
      config: { categories: ['heart', 'breath', 'total'] },
    } as MarkSpec;
    const result = await loadManifest(ds([spec]));
    const mark = result.marks[0]!;
    const before = innerUuid(mark.group);

    (spec.data as any).distributions = [
      [20, 22, 24, 22, 20],
      [10, 11, 12, 11, 10],
      [ 5,  5,  5,  5,  5],
    ];
    mark.refresh!(spec);

    expect(innerUuid(mark.group)).not.toBe(before);
    result.dispose();
  });

  it('emits no layer meshes when distributions are empty', async () => {
    const result = await loadManifest(ds([{
      id: 'phases', type: 'streamgraph', title: 'Phases',
      data: { source: 'inline', distributions: [] },
      config: { categories: [] },
    } as MarkSpec]));
    const mark = result.marks[0]!;
    expect(mark.group.name).toBe('streamgraph:phases');
    expect(meshCount(mark.group)).toBe(0);
    result.dispose();
  });
});

/* ─── unknown mark type ──────────────────────────────────────────────── */

describe('builder dispatch', () => {
  it('skips marks whose type has no builder registered', async () => {
    const result = await loadManifest(ds([
      { id: 'good', type: 'line', title: 'Good',
        data: { source: 'inline', series: sampleSeries } } as MarkSpec,
      { id: 'bad', type: 'never-registered' as any, title: 'Bad',
        data: { source: 'inline' } } as MarkSpec,
    ]));
    expect(result.marks).toHaveLength(1);
    expect(result.marks[0]!.id).toBe('good');
    result.dispose();
  });
});
