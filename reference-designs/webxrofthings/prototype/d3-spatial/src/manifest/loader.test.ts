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
    expect(fetchMock).toHaveBeenCalledWith('/api/v1/test', { headers: {} });

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

    expect(fetchMock).toHaveBeenCalledWith('/api', {
      headers: { Authorization: 'Bearer tok-123' },
    });
    result.dispose();
  });

  it('skips a mark whose URL returns non-OK', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchOnce({}, false);

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/oops', shape: 'series' },
    } as MarkSpec]));

    expect(result.marks).toHaveLength(0);
    expect(rec.initialCalls).toHaveLength(0);
    result.dispose();
  });

  it('skips a mark whose URL fetch rejects (network error)', async () => {
    const rec = makeRecordingBuilder();
    registerMarkBuilder(TEST_TYPE, rec.builder);
    mockFetchRejectOnce(new Error('ECONNREFUSED'));

    const result = await loadManifest(ds([{
      id: 'm1', type: TEST_TYPE, title: 'm',
      data: { source: 'url', url: '/down', shape: 'series' },
    } as MarkSpec]));

    expect(result.marks).toHaveLength(0);
    expect(rec.initialCalls).toHaveLength(0);
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
