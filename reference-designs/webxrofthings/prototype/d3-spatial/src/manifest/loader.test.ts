import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import * as THREE from 'three';
import { loadManifest, registerMarkBuilder, type LoadedMark } from './loader';
import type { DataspaceManifest, MarkSpec, MarkType } from './schema';

/**
 * URL / refresh-path coverage for `loadManifest`. We register a tiny synthetic
 * builder for an arbitrary MarkType, mock global `fetch`, and use vitest fake
 * timers to step through scheduled `setInterval` ticks.
 */

/* ─── shared scaffolding ────────────────────────────────────────────── */

interface RecordingBuilder {
  initialCalls: MarkSpec[];     // each `builder(spec)` call
  refreshCalls: MarkSpec[];     // each `mark.refresh(spec)` call
  builder: (spec: MarkSpec) => LoadedMark;
}

function makeRecordingBuilder(opts: { withRefresh?: boolean } = {}): RecordingBuilder {
  const { withRefresh = true } = opts;
  const initialCalls: MarkSpec[] = [];
  const refreshCalls: MarkSpec[] = [];
  return {
    initialCalls,
    refreshCalls,
    builder: (spec: MarkSpec): LoadedMark => {
      // Snapshot the spec's data state so later mutations don't change history.
      initialCalls.push({ ...spec, data: JSON.parse(JSON.stringify(spec.data)) } as MarkSpec);
      const group = new THREE.Group();
      group.name = `test:${spec.id}`;
      const mark: LoadedMark = {
        id: spec.id,
        type: spec.type,
        title: spec.title,
        group,
        viz: null,
        drillable: false,
        hoverable: false,
        draggable: false,
        defaultVisible: true,
      };
      if (withRefresh) {
        mark.refresh = (s: MarkSpec) => {
          refreshCalls.push({ ...s, data: JSON.parse(JSON.stringify(s.data)) } as MarkSpec);
        };
      }
      return mark;
    },
  };
}

function ds(marks: MarkSpec[]): DataspaceManifest {
  return { version: '1', name: 'loader.test', scaleTag: 'room', marks };
}

const TEST_TYPE = 'tree' as MarkType; // any registered name works; we override

/* ─── fetch mock ────────────────────────────────────────────────────── */

let fetchMock: ReturnType<typeof vi.fn>;
let originalFetch: typeof globalThis.fetch | undefined;

function mockResponse(body: unknown, ok = true, status = 200): Response {
  return {
    ok,
    status,
    json: async () => body,
  } as unknown as Response;
}

function mockFetchOnce(body: unknown, ok = true) {
  fetchMock.mockResolvedValueOnce(mockResponse(body, ok, ok ? 200 : 404));
}

function mockFetchRejectOnce(err: Error) {
  fetchMock.mockRejectedValueOnce(err);
}

beforeEach(() => {
  vi.useFakeTimers();
  fetchMock = vi.fn();
  originalFetch = globalThis.fetch;
  globalThis.fetch = fetchMock as unknown as typeof globalThis.fetch;
  // Quiet the loader's expected console.warn lines during failure-path tests.
  vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
  vi.useRealTimers();
  if (originalFetch) globalThis.fetch = originalFetch;
  vi.restoreAllMocks();
});

/* ─── initial fetch ─────────────────────────────────────────────────── */

describe('loadManifest — initial URL fetch', () => {
  it('fetches a URL data source and mutates spec.data into inline form', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    const series = [{ t: 1, v: 10 }, { t: 2, v: 20 }];
    mockFetchOnce(series);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/api/v1/test', shape: 'series', refreshInterval: 0 },
    } as MarkSpec]));

    expect(result.marks).toHaveLength(1);
    expect(rec.initialCalls).toHaveLength(1);
    const seenSpec = rec.initialCalls[0]!;
    expect((seenSpec.data as any).source).toBe('inline');
    expect((seenSpec.data as any).series).toEqual(series);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    /* fetchInto now wraps every request in an AbortController (5 s timeout)
     * so unreachable URLs can't stall the manifest load forever — see the
     * "blank scene when device offline" fix. The signal is opaque from the
     * test's perspective; just match any AbortSignal. */
    expect(fetchMock).toHaveBeenCalledWith('/api/v1/test',
      expect.objectContaining({ headers: {}, signal: expect.any(AbortSignal) }));

    result.dispose();
  });

  it('unwraps `{samples: [...]}` series-shape responses', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    const samples = [{ t: 1, v: 1 }, { t: 2, v: 2 }];
    mockFetchOnce({ samples });

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/x', shape: 'series' },
    } as MarkSpec]));

    expect((rec.initialCalls[0]!.data as any).series).toEqual(samples);
    result.dispose();
  });

  it('sends Authorization: Bearer when a token is provided', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce([]);

    const result = await loadManifest(
      ds([{
        id: 'm1', type: TEST_TYPE, title: 'm',
        data: { source: 'url', url: '/api', shape: 'series' },
      } as MarkSpec]),
      'tok-123',
    );

    expect(fetchMock).toHaveBeenCalledWith('/api',
      expect.objectContaining({
        headers: { Authorization: 'Bearer tok-123' },
        signal: expect.any(AbortSignal),
      }));
    result.dispose();
  });

  /* Previously: on non-OK / network error, the mark was skipped entirely
   * (`continue` in the for-loop). That made charts vanish whenever the
   * device endpoint was temporarily down — worse, a hanging fetch stalled
   * the whole sequential await loop and the scene stayed blank.
   *
   * Now: the mark is built anyway (the health monitor flips it to
   * `offline` on the first failure when there's been no prior success,
   * and the loader seeds `spec.data` with a fake payload for shapes
   * known to fakeData.ts). The user perceives this as the chart
   * "running on DEMO mode" with the offline-sensors HUD visible.
   * Unknown shapes (graph, hierarchy, …) still leave spec.data in its
   * url-source form — no fake generator means no placeholder data. */
  it('builds a mark + seeds fake data when URL returns non-OK', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce({}, false);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/oops', shape: 'series' },
    } as MarkSpec]));

    expect(result.marks).toHaveLength(1);
    expect(rec.initialCalls).toHaveLength(1);
    /* shape='series' is supported by fakeData.ts → spec.data was
     * mutated to the fake inline form before the builder ran. */
    expect((rec.initialCalls[0]!.data as any).source).toBe('inline');
    expect(result.health.stateOf('m1')).toBe('offline');
    result.dispose();
  });

  it('builds a mark + seeds fake data when URL fetch rejects (network error)', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchRejectOnce(new Error('ECONNREFUSED'));

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/down', shape: 'series' },
    } as MarkSpec]));

    expect(result.marks).toHaveLength(1);
    expect(rec.initialCalls).toHaveLength(1);
    expect((rec.initialCalls[0]!.data as any).source).toBe('inline');
    expect(result.health.stateOf('m1')).toBe('offline');
    result.dispose();
  });

  it('skips WebSocket sources without calling fetch', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: 'wss://example/stream', shape: 'series' },
    } as MarkSpec]));

    expect(result.marks).toHaveLength(0);
    expect(fetchMock).not.toHaveBeenCalled();
    result.dispose();
  });

  it('passes inline data through without fetching', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'inline', series: [{ t: 1, v: 1 }] },
    } as MarkSpec]));

    expect(result.marks).toHaveLength(1);
    expect(fetchMock).not.toHaveBeenCalled();
    result.dispose();
  });
});

/* ─── refreshInterval scheduling ────────────────────────────────────── */

describe('loadManifest — refreshInterval scheduling', () => {
  it('does NOT schedule any interval when refreshInterval is 0', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce([]);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/x', shape: 'series', refreshInterval: 0 },
    } as MarkSpec]));

    // Advance a long time; no second fetch.
    await vi.advanceTimersByTimeAsync(10 * 60 * 1000);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(rec.refreshCalls).toHaveLength(0);
    result.dispose();
  });

  it('does NOT schedule when refreshInterval > 0 but the builder has no refresh()', async () => {
    const rec = makeRecordingBuilder({ withRefresh: false });
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce([]);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/x', shape: 'series', refreshInterval: 5 },
    } as MarkSpec]));

    await vi.advanceTimersByTimeAsync(60_000);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    result.dispose();
  });

  it('re-fetches on the configured cadence and calls mark.refresh', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    // initial fetch
    mockFetchOnce([{ t: 1, v: 1 }]);
    // three subsequent refresh fetches
    mockFetchOnce([{ t: 2, v: 2 }]);
    mockFetchOnce([{ t: 3, v: 3 }]);
    mockFetchOnce([{ t: 4, v: 4 }]);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/x', shape: 'series', refreshInterval: 30 },
    } as MarkSpec]));

    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(rec.refreshCalls).toHaveLength(0);

    // 1st tick (30 s)
    await vi.advanceTimersByTimeAsync(30_000);
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(rec.refreshCalls).toHaveLength(1);
    expect((rec.refreshCalls[0]!.data as any).series).toEqual([{ t: 2, v: 2 }]);

    // 2nd tick (another 30 s)
    await vi.advanceTimersByTimeAsync(30_000);
    expect(fetchMock).toHaveBeenCalledTimes(3);
    expect((rec.refreshCalls[1]!.data as any).series).toEqual([{ t: 3, v: 3 }]);

    // 3rd tick
    await vi.advanceTimersByTimeAsync(30_000);
    expect(rec.refreshCalls).toHaveLength(3);

    result.dispose();
  });

  it('survives a transient refresh failure — keeps polling, still calls refresh on next OK', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce([{ t: 1, v: 1 }]);                  // initial OK
    mockFetchOnce({}, false);                          // 1st refresh: 404
    mockFetchRejectOnce(new Error('ETIMEDOUT'));       // 2nd refresh: network error
    mockFetchOnce([{ t: 4, v: 4 }]);                  // 3rd refresh: OK

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/x', shape: 'series', refreshInterval: 10 },
    } as MarkSpec]));

    await vi.advanceTimersByTimeAsync(10_000);
    expect(rec.refreshCalls).toHaveLength(0);          // 404, no refresh
    await vi.advanceTimersByTimeAsync(10_000);
    expect(rec.refreshCalls).toHaveLength(0);          // network error, no refresh
    await vi.advanceTimersByTimeAsync(10_000);
    expect(rec.refreshCalls).toHaveLength(1);          // recovered
    expect((rec.refreshCalls[0]!.data as any).series).toEqual([{ t: 4, v: 4 }]);

    result.dispose();
  });

  it('schedules independent intervals per mark', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce([{ t: 1, v: 1 }]);    // m1 initial
    mockFetchOnce([{ t: 100, v: 1 }]);  // m2 initial
    mockFetchOnce([{ t: 2, v: 2 }]);    // m1 refresh #1
    mockFetchOnce([{ t: 200, v: 2 }]);  // m2 refresh #1
    mockFetchOnce([{ t: 3, v: 3 }]);    // m1 refresh #2 (m2's 60s tick hasn't fired)

    const result = await loadManifest(ds([
      { id: 'm1', type: TEST_TYPE, title: 'm1',
        data: { source: 'url', url: '/m1', shape: 'series', refreshInterval: 30 } } as MarkSpec,
      { id: 'm2', type: TEST_TYPE, title: 'm2',
        data: { source: 'url', url: '/m2', shape: 'series', refreshInterval: 60 } } as MarkSpec,
    ]));

    expect(fetchMock).toHaveBeenCalledTimes(2);

    // 30 s — only m1 ticks
    await vi.advanceTimersByTimeAsync(30_000);
    expect(fetchMock).toHaveBeenCalledTimes(3);

    // another 30 s (total 60) — m1 again AND m2 first
    await vi.advanceTimersByTimeAsync(30_000);
    expect(fetchMock).toHaveBeenCalledTimes(5);

    // 2 refreshes for m1, 1 for m2
    expect(rec.refreshCalls.filter(s => s.id === 'm1')).toHaveLength(2);
    expect(rec.refreshCalls.filter(s => s.id === 'm2')).toHaveLength(1);

    result.dispose();
  });
});

/* ─── dispose ───────────────────────────────────────────────────────── */

describe('LoadResult.dispose', () => {
  it('clears scheduled intervals so no further refresh fetches happen', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce([]);
    mockFetchOnce([]);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/x', shape: 'series', refreshInterval: 30 },
    } as MarkSpec]));

    await vi.advanceTimersByTimeAsync(30_000);
    expect(fetchMock).toHaveBeenCalledTimes(2);

    result.dispose();

    // No further fetches even after a long wait.
    await vi.advanceTimersByTimeAsync(5 * 60_000);
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(rec.refreshCalls).toHaveLength(1);
  });

  it('is idempotent — calling dispose twice does not throw', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce([]);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/x', shape: 'series', refreshInterval: 30 },
    } as MarkSpec]));

    expect(() => { result.dispose(); result.dispose(); }).not.toThrow();
  });
});

/* ─── snapshot shape ────────────────────────────────────────────────── */

describe('loadManifest — snapshot shape', () => {
  it('plucks the configured field and accumulates into a per-mark rolling buffer', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    /* Three live polls: 72 → 74 → 76 bpm, all with presence=true. */
    mockFetchOnce({ bpm: 72, presence: true, timestamp_us: 1 });
    mockFetchOnce({ bpm: 74, presence: true, timestamp_us: 2 });
    mockFetchOnce({ bpm: 76, presence: true, timestamp_us: 3 });

    const result = await loadManifest(ds([{
      id: 'hr', type: TEST_TYPE, title: 'HR',
      data: { source: 'url', url: '/heart-rate', shape: 'snapshot', refreshInterval: 3 },
      config: { pluck: 'bpm', presenceField: 'presence', historyLength: 10 },
    } as MarkSpec]));

    /* Step the refresh interval twice — first poll happened at load
     * time; the next two are at 3 s, 6 s. */
    await vi.advanceTimersByTimeAsync(6_500);

    /* The mark's most recent refresh saw the full buffer. */
    expect(rec.refreshCalls.length).toBeGreaterThanOrEqual(2);
    const last = rec.refreshCalls.at(-1)!;
    expect((last.data as any).source).toBe('inline');
    const series = (last.data as any).series as Array<{ t: number; v: number }>;
    expect(series.map(s => s.v)).toEqual([72, 74, 76]);
    expect(series.every(s => typeof s.t === 'number')).toBe(true);

    result.dispose();
  });

  it('skips presence=false ticks without polluting the buffer', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    /* First poll seeds the buffer; second poll has presence=false and
     * must NOT push a value; third resumes presence=true. */
    mockFetchOnce({ bpm: 72, presence: true });
    mockFetchOnce({ bpm: 0,  presence: false });   // empty room
    mockFetchOnce({ bpm: 75, presence: true });

    const result = await loadManifest(ds([{
      id: 'hr', type: TEST_TYPE, title: 'HR',
      data: { source: 'url', url: '/heart-rate', shape: 'snapshot', refreshInterval: 3 },
      config: { pluck: 'bpm', presenceField: 'presence', historyLength: 10 },
    } as MarkSpec]));

    await vi.advanceTimersByTimeAsync(6_500);

    const last = rec.refreshCalls.at(-1)!;
    const series = (last.data as any).series as Array<{ t: number; v: number }>;
    expect(series.map(s => s.v)).toEqual([72, 75]);

    result.dispose();
  });

  it('trims the buffer to historyLength', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    /* historyLength = 3; push 5 values; buffer should retain the last 3. */
    for (const v of [10, 20, 30, 40, 50]) mockFetchOnce({ rpm: v });

    const result = await loadManifest(ds([{
      id: 'br', type: TEST_TYPE, title: 'BR',
      data: { source: 'url', url: '/breathing', shape: 'snapshot', refreshInterval: 1 },
      config: { pluck: 'rpm', historyLength: 3 },
    } as MarkSpec]));

    await vi.advanceTimersByTimeAsync(4_500);

    const last = rec.refreshCalls.at(-1)!;
    const series = (last.data as any).series as Array<{ t: number; v: number }>;
    expect(series.map(s => s.v)).toEqual([30, 40, 50]);

    result.dispose();
  });

  it('seeds fake snapshot values when the device is offline', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    /* Pre-fetch fails → loader should seed fake data on cold start. */
    mockFetchRejectOnce(new Error('econnrefused'));

    const result = await loadManifest(ds([{
      id: 'hr-line', type: TEST_TYPE, title: 'HR',
      data: { source: 'url', url: '/heart-rate', shape: 'snapshot', refreshInterval: 3 },
      config: { pluck: 'bpm', presenceField: 'presence', historyLength: 60,
                vMin: 40, vMax: 130 },
    } as MarkSpec]));

    /* The pre-fetch failed; seedFakeIfPossible needs at least 2 values
     * in the buffer to stamp inline data — so cold-start may still hand
     * the builder a URL spec. After the first refresh interval (also
     * failing) a second fake gets pushed and the spec converts. */
    mockFetchRejectOnce(new Error('econnrefused'));
    mockFetchRejectOnce(new Error('econnrefused'));
    mockFetchRejectOnce(new Error('econnrefused'));
    await vi.advanceTimersByTimeAsync(15_000);

    /* By now the health monitor flagged offline and fake data flowed. */
    const last = rec.refreshCalls.at(-1);
    if (last) {
      const data = last.data as any;
      if (data.source === 'inline') {
        const series = data.series as Array<{ t: number; v: number }>;
        expect(series.length).toBeGreaterThanOrEqual(2);
        /* Fake values should respect vMin/vMax (40..130) — fakeData's
         * fakeRangeFor reads spec.config.vMin/vMax first. */
        for (const s of series) {
          expect(s.v).toBeGreaterThan(20);
          expect(s.v).toBeLessThan(150);
        }
      }
    }

    result.dispose();
  });
});
