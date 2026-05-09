import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import * as THREE from 'three';
import { buildLiveLineCell, buildLivePhasesCell, buildLiveTargetsCell } from './liveVitalsCells';

/* ─── shared scaffolding ────────────────────────────────────────────── */

let fetchMock: ReturnType<typeof vi.fn>;
let originalFetch: typeof globalThis.fetch | undefined;

function mockResponseOnce(body: unknown, ok = true, status = 200) {
  fetchMock.mockResolvedValueOnce({
    ok, status,
    json: async () => body,
  } as unknown as Response);
}

function mockRejectOnce(err: Error) {
  fetchMock.mockRejectedValueOnce(err);
}

beforeEach(() => {
  vi.useFakeTimers();
  fetchMock = vi.fn();
  originalFetch = globalThis.fetch;
  globalThis.fetch = fetchMock as unknown as typeof globalThis.fetch;
});

afterEach(() => {
  vi.useRealTimers();
  if (originalFetch) globalThis.fetch = originalFetch;
  vi.restoreAllMocks();
});

/* Walk the tree and count Mesh + Line objects (THREE.Line counts as a mesh-like
 * for our presence checks below — the live cells produce a mix of both). */
function countDrawables(root: THREE.Object3D): number {
  let n = 0;
  root.traverse(o => {
    const a = o as THREE.Mesh & THREE.Line;
    if (a.isMesh || a.isLine) n++;
  });
  return n;
}

function uuidsOf(root: THREE.Object3D): string[] {
  const ids: string[] = [];
  root.traverse(o => { ids.push(o.uuid); });
  return ids;
}

/* ─── buildLiveLineCell ─────────────────────────────────────────────── */

describe('buildLiveLineCell', () => {
  const URL = '/api/v1/vitals/heart-rate/history';

  it('starts with a placeholder line; first fetch swaps in real data', async () => {
    mockResponseOnce({ samples: [
      { t: 1000, v: 60 }, { t: 2000, v: 70 }, { t: 3000, v: 80 },
    ] });
    const cell = buildLiveLineCell({ url: URL, refreshMs: 30000 });

    // Build is synchronous; placeholder is in place immediately.
    expect(cell.group.name).toBe(`live-line:${URL}`);
    expect(countDrawables(cell.group)).toBeGreaterThan(0);
    const placeholderUuids = uuidsOf(cell.group);

    // Allow microtasks to drain so the initial fetch resolves.
    await vi.advanceTimersByTimeAsync(0);

    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock).toHaveBeenCalledWith(URL);
    // After the swap the inner group should be a different THREE object.
    const afterUuids = uuidsOf(cell.group);
    expect(afterUuids).not.toEqual(placeholderUuids);

    cell.dispose();
  });

  it('keeps polling at refreshMs cadence, swapping content each time', async () => {
    mockResponseOnce({ samples: [{ t: 1, v: 60 }, { t: 2, v: 65 }] });   // initial
    mockResponseOnce({ samples: [{ t: 1, v: 80 }, { t: 2, v: 82 }] });   // tick 1
    mockResponseOnce({ samples: [{ t: 1, v: 90 }, { t: 2, v: 92 }] });   // tick 2

    const cell = buildLiveLineCell({ url: URL, refreshMs: 5000 });
    await vi.advanceTimersByTimeAsync(0);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const afterInitial = uuidsOf(cell.group);

    await vi.advanceTimersByTimeAsync(5000);
    expect(fetchMock).toHaveBeenCalledTimes(2);
    const afterTick1 = uuidsOf(cell.group);
    expect(afterTick1).not.toEqual(afterInitial);

    await vi.advanceTimersByTimeAsync(5000);
    expect(fetchMock).toHaveBeenCalledTimes(3);

    cell.dispose();
  });

  it('accepts a bare array (not just `{samples: [...]}`)', async () => {
    mockResponseOnce([{ t: 1, v: 1 }, { t: 2, v: 2 }, { t: 3, v: 3 }]);
    const cell = buildLiveLineCell({ url: URL, refreshMs: 30000 });
    await vi.advanceTimersByTimeAsync(0);

    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(countDrawables(cell.group)).toBeGreaterThan(0);
    cell.dispose();
  });

  it('keeps the placeholder when the fetch returns < 2 samples', async () => {
    mockResponseOnce({ samples: [{ t: 1, v: 1 }] });   // only one sample
    const cell = buildLiveLineCell({ url: URL, refreshMs: 30000 });
    const placeholderUuids = uuidsOf(cell.group);

    await vi.advanceTimersByTimeAsync(0);
    expect(uuidsOf(cell.group)).toEqual(placeholderUuids);
    cell.dispose();
  });

  it('keeps the previous frame on transient fetch failure', async () => {
    mockResponseOnce({ samples: [{ t: 1, v: 60 }, { t: 2, v: 70 }] });   // initial OK
    mockResponseOnce({}, false);                                          // 404
    mockRejectOnce(new Error('ETIMEDOUT'));                               // network error

    const cell = buildLiveLineCell({ url: URL, refreshMs: 1000 });
    await vi.advanceTimersByTimeAsync(0);
    const afterInitial = uuidsOf(cell.group);

    await vi.advanceTimersByTimeAsync(1000);   // 404 — no swap
    expect(uuidsOf(cell.group)).toEqual(afterInitial);

    await vi.advanceTimersByTimeAsync(1000);   // network error — still no swap
    expect(uuidsOf(cell.group)).toEqual(afterInitial);

    cell.dispose();
  });

  it('dispose() stops the polling loop', async () => {
    mockResponseOnce({ samples: [{ t: 1, v: 1 }, { t: 2, v: 2 }] });
    const cell = buildLiveLineCell({ url: URL, refreshMs: 1000 });
    await vi.advanceTimersByTimeAsync(0);
    expect(fetchMock).toHaveBeenCalledTimes(1);

    cell.dispose();
    await vi.advanceTimersByTimeAsync(60_000);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it('exposes a tick() that is a no-op (line is static between refreshes)', async () => {
    mockResponseOnce([]);
    const cell = buildLiveLineCell({ url: URL, refreshMs: 1000 });
    expect(typeof cell.tick).toBe('function');
    expect(() => cell.tick(1.234)).not.toThrow();
    cell.dispose();
  });
});

/* ─── buildLivePhasesCell ───────────────────────────────────────────── */

describe('buildLivePhasesCell', () => {
  const URL = '/api/v1/vitals/phases';

  /* /phases response shape: [[heart…], [breath…], [total…]]. */
  const validPhases: number[][] = [
    [1, 2, 3, 4, 5],
    [2, 3, 4, 3, 2],
    [3, 3, 3, 3, 3],
  ];

  it('starts with a placeholder; first fetch swaps in a streamgraph', async () => {
    mockResponseOnce(validPhases);
    const cell = buildLivePhasesCell({ url: URL, refreshMs: 1500, windowSize: 30 });
    expect(cell.group.name).toBe(`live-phases:${URL}`);
    const placeholderUuids = uuidsOf(cell.group);

    await vi.advanceTimersByTimeAsync(0);

    expect(fetchMock).toHaveBeenCalledTimes(1);
    const afterUuids = uuidsOf(cell.group);
    expect(afterUuids).not.toEqual(placeholderUuids);
  });

  it('rebuilds on each refresh tick', async () => {
    mockResponseOnce(validPhases);
    mockResponseOnce(validPhases.map(row => row.map(v => v + 10)));
    mockResponseOnce(validPhases.map(row => row.map(v => v - 5)));

    const cell = buildLivePhasesCell({ url: URL, refreshMs: 1000 });
    await vi.advanceTimersByTimeAsync(0);
    const t0 = uuidsOf(cell.group);

    await vi.advanceTimersByTimeAsync(1000);
    const t1 = uuidsOf(cell.group);
    expect(t1).not.toEqual(t0);

    await vi.advanceTimersByTimeAsync(1000);
    const t2 = uuidsOf(cell.group);
    expect(t2).not.toEqual(t1);

    cell.dispose();
  });

  it('skips swap when distributions are too short or empty', async () => {
    mockResponseOnce([]);                            // empty
    mockResponseOnce([[1]]);                         // single value
    const cell = buildLivePhasesCell({ url: URL, refreshMs: 1000 });
    const placeholderUuids = uuidsOf(cell.group);

    await vi.advanceTimersByTimeAsync(0);
    expect(uuidsOf(cell.group)).toEqual(placeholderUuids);

    await vi.advanceTimersByTimeAsync(1000);
    expect(uuidsOf(cell.group)).toEqual(placeholderUuids);

    cell.dispose();
  });

  it('tick() forwards to the inner streamgraph viz when present', async () => {
    mockResponseOnce(validPhases);
    const cell = buildLivePhasesCell({ url: URL, refreshMs: 1500, windowSize: 30 });
    // Tick before the streamgraph is built — must not throw.
    expect(() => cell.tick(0)).not.toThrow();

    await vi.advanceTimersByTimeAsync(0);
    // Now the streamgraph exists; tick exercises its scrolling.
    expect(() => cell.tick(0.5)).not.toThrow();
    cell.dispose();
  });

  it('dispose() stops polling', async () => {
    mockResponseOnce(validPhases);
    const cell = buildLivePhasesCell({ url: URL, refreshMs: 500 });
    await vi.advanceTimersByTimeAsync(0);
    expect(fetchMock).toHaveBeenCalledTimes(1);

    cell.dispose();
    await vi.advanceTimersByTimeAsync(10_000);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });
});

/* ─── buildLiveTargetsCell ──────────────────────────────────────────── */

describe('buildLiveTargetsCell', () => {
  const URL = '/api/v1/vitals/targets';

  it('renders the static cone + range arc + radar marker before any fetch', async () => {
    mockResponseOnce({ count: 0, targets: [] });
    const cell = buildLiveTargetsCell({ url: URL, refreshMs: 1000 });
    expect(cell.group.name).toBe(`live-targets:${URL}`);
    /* Cone (LineSegments) + range arc (Line) + radar marker (Mesh) + 3 hidden
     * target glyph meshes = 6 drawables minimum. */
    expect(countDrawables(cell.group)).toBeGreaterThanOrEqual(6);
    cell.dispose();
  });

  it('shows glyphs at fetched target positions and hides the rest', async () => {
    mockResponseOnce({
      count: 2,
      targets: [
        { id: 0, x_m: 0.0,  y_m: 1.0 },
        { id: 1, x_m: 0.4,  y_m: 1.5 },
      ],
    });
    const cell = buildLiveTargetsCell({ url: URL, refreshMs: 1000, extent_m: 2.5, max_distance_m: 4.0 });
    await vi.advanceTimersByTimeAsync(0);

    const visibleMeshes = (cell.group.children as THREE.Mesh[])
      .filter(c => (c as THREE.Mesh).isMesh && c.visible);
    /* radar marker (always visible) + 2 target glyphs = 3. */
    expect(visibleMeshes.length).toBe(3);
    cell.dispose();
  });

  it('hides all glyphs when fetch errors', async () => {
    mockRejectOnce(new Error('disconnected'));
    const cell = buildLiveTargetsCell({ url: URL, refreshMs: 1000 });
    await vi.advanceTimersByTimeAsync(0);
    const visibleGlyphs = (cell.group.children as THREE.Mesh[])
      .filter(c => (c as THREE.Mesh).isMesh && c.visible && c.geometry instanceof THREE.SphereGeometry);
    expect(visibleGlyphs.length).toBe(0);
    cell.dispose();
  });

  it('updates positions on each refresh tick', async () => {
    mockResponseOnce({ count: 1, targets: [{ id: 0, x_m: 0.0, y_m: 1.0 }] });
    mockResponseOnce({ count: 1, targets: [{ id: 0, x_m: 0.5, y_m: 2.0 }] });
    const cell = buildLiveTargetsCell({ url: URL, refreshMs: 1000 });

    await vi.advanceTimersByTimeAsync(0);
    const glyph = (cell.group.children as THREE.Mesh[])
      .find(c => (c as THREE.Mesh).isMesh && c.geometry instanceof THREE.SphereGeometry && c.visible);
    expect(glyph).toBeDefined();
    const pos1 = glyph!.position.clone();

    await vi.advanceTimersByTimeAsync(1000);
    const pos2 = glyph!.position.clone();
    expect(pos2.equals(pos1)).toBe(false);

    cell.dispose();
  });

  it('caps glyphs at MAX_TARGETS (3) even if more are returned', async () => {
    mockResponseOnce({
      count: 5,
      targets: [
        { id: 0, x_m: 0, y_m: 1 }, { id: 1, x_m: 0.1, y_m: 1.2 },
        { id: 2, x_m: 0.2, y_m: 1.4 }, { id: 3, x_m: 0.3, y_m: 1.6 },
        { id: 4, x_m: 0.4, y_m: 1.8 },
      ],
    });
    const cell = buildLiveTargetsCell({ url: URL, refreshMs: 1000 });
    await vi.advanceTimersByTimeAsync(0);
    const visibleGlyphs = (cell.group.children as THREE.Mesh[])
      .filter(c => (c as THREE.Mesh).isMesh && c.geometry instanceof THREE.SphereGeometry && c.visible);
    expect(visibleGlyphs.length).toBe(3);
    cell.dispose();
  });

  it('dispose() stops polling and frees pre-allocated geometries', async () => {
    mockResponseOnce({ count: 0, targets: [] });
    const cell = buildLiveTargetsCell({ url: URL, refreshMs: 500 });
    await vi.advanceTimersByTimeAsync(0);
    expect(fetchMock).toHaveBeenCalledTimes(1);

    cell.dispose();
    await vi.advanceTimersByTimeAsync(10_000);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });
});
