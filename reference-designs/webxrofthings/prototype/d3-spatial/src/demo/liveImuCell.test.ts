import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import * as THREE from 'three';

// Mock troika-three-text — the IMU cell uses it for the heading + airspeed
// instrument labels, but the library expects browser `self`/`Worker` globals
// that node's vitest env doesn't have. Stub is the same shape as the one in
// liveSplatGalleryCell.test.ts.
vi.mock('troika-three-text', () => {
  class StubText extends THREE.Object3D {
    text = '';
    fontSize = 0.018;
    color: number = 0xffffff;
    anchorX: string = 'center';
    anchorY: string = 'middle';
    sync() {}
    dispose() {}
  }
  return { Text: StubText };
});

// three-globe is dynamically imported by liveImuCell, gated on
// `typeof window !== 'undefined'`. Node-env vitest leaves window
// undefined so the globe import never fires here — no mock needed.

import { buildLiveImuCell, type ImuSnapshot } from './liveImuCell';

/** The airplane mesh used to be a direct child of cell.group; after the
 *  left/right split it sits inside the `instrument-panel` wrapper, so the
 *  tests traverse rather than indexing by direct child. */
function findByName(root: THREE.Object3D, name: string): THREE.Object3D | undefined {
  let hit: THREE.Object3D | undefined;
  root.traverse(o => { if (!hit && o.name === name) hit = o; });
  return hit;
}

const URL = '/api/v1/sensor/imu';
const REFRESH = 1000;

let fetchMock: ReturnType<typeof vi.fn>;
let originalFetch: typeof globalThis.fetch | undefined;

function mockResponseOnce(body: unknown, ok = true) {
  fetchMock.mockResolvedValueOnce({ ok, status: ok ? 200 : 500, json: async () => body } as unknown as Response);
}
function mockRejectOnce(err: Error) {
  fetchMock.mockRejectedValueOnce(err);
}
function snapshot(roll: number, pitch: number, yaw: number): ImuSnapshot {
  return {
    orientation: { roll_rad: roll, pitch_rad: pitch, yaw_rad: yaw },
    angular_velocity: { x: 0, y: 0, z: 0 },
    acceleration:     { x: 0, y: 9.81, z: 0 },
  };
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

describe('buildLiveImuCell', () => {
  it('exposes the LiveCell-shaped API (group + tick + dispose + getStatus)', async () => {
    mockResponseOnce(snapshot(0, 0, 0));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH });
    expect(cell.group).toBeInstanceOf(THREE.Group);
    expect(typeof cell.tick).toBe('function');
    expect(typeof cell.dispose).toBe('function');
    expect(typeof cell.getStatus).toBe('function');
    cell.dispose();
  });

  it('builds an airplane mesh composed of cheap THREE primitives', async () => {
    mockResponseOnce(snapshot(0, 0, 0));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH });
    // Inner "airplane-mesh" group contains 4 box meshes: fuselage, wings, tail, stabilizer.
    const airplane = findByName(cell.group, 'airplane-mesh');
    expect(airplane).toBeDefined();
    expect(airplane!.children.length).toBe(4);
    for (const m of airplane!.children) {
      expect((m as THREE.Mesh).isMesh).toBe(true);
    }
    cell.dispose();
  });

  it('rotates the airplane toward the latest IMU orientation', async () => {
    // First fetch: ~no rotation. Second: 0.5 rad roll. After the second
    // fetch resolves, the target quaternion should reflect the new attitude.
    mockResponseOnce(snapshot(0, 0, 0));
    mockResponseOnce(snapshot(0.5, 0, 0));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH, smooth: false });
    cell.setActive(true);                              // start polling
    await vi.advanceTimersByTimeAsync(0);             // first response

    const airplane = findByName(cell.group, 'airplane-mesh')!;
    const initialQuat = airplane.quaternion.clone();

    await vi.advanceTimersByTimeAsync(REFRESH);       // second response
    // With smooth: false the airplane snaps directly to the new orientation
    // on each fetch — quaternion must have changed.
    expect(airplane.quaternion.equals(initialQuat)).toBe(false);
    cell.dispose();
  });

  it('keeps the airplane at its last orientation on transient fetch failure', async () => {
    mockResponseOnce(snapshot(0.5, 0, 0));     // initial OK
    mockRejectOnce(new Error('disconnect'));   // network error
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH, smooth: false });
    cell.setActive(true);
    await vi.advanceTimersByTimeAsync(0);

    const airplane = findByName(cell.group, 'airplane-mesh')!;
    const afterInitial = airplane.quaternion.clone();
    await vi.advanceTimersByTimeAsync(REFRESH);

    // No change despite the failed fetch.
    expect(airplane.quaternion.equals(afterInitial)).toBe(true);
    cell.dispose();
  });

  it('ignores malformed payloads (treats them as live, no movement)', async () => {
    // Returns a 200 but with missing orientation — should not throw, should
    // not move the airplane, status should remain `live` after success.
    mockResponseOnce({ acceleration: { x: 0, y: 0, z: 0 } });
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH, smooth: false });
    cell.setActive(true);
    await vi.advanceTimersByTimeAsync(0);

    const airplane = findByName(cell.group, 'airplane-mesh')!;
    // Identity quaternion — never moved.
    expect(airplane.quaternion.x).toBe(0);
    expect(airplane.quaternion.y).toBe(0);
    expect(airplane.quaternion.z).toBe(0);
    expect(airplane.quaternion.w).toBe(1);
    expect(cell.getStatus().state).toBe('live');
    cell.dispose();
  });

  it('reports live status after the first successful poll', async () => {
    mockResponseOnce(snapshot(0, 0, 0));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH });
    // Inactive cell has no poller — getStatus() reports offline by design.
    expect(cell.getStatus().state).toBe('offline');
    cell.setActive(true);
    expect(cell.getStatus().state).toBe('live');   // fresh poller, never failed
    await vi.advanceTimersByTimeAsync(0);
    expect(cell.getStatus().state).toBe('live');
    cell.dispose();
  });

  it('reports offline after two consecutive errors with no success', async () => {
    mockRejectOnce(new Error('boom'));
    mockRejectOnce(new Error('boom'));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH });
    cell.setActive(true);
    await vi.advanceTimersByTimeAsync(0);             // err 1
    expect(cell.getStatus().state).toBe('stale');
    await vi.advanceTimersByTimeAsync(REFRESH);       // err 2 → offline
    expect(cell.getStatus().state).toBe('offline');
    cell.dispose();
  });

  it('dispose() stops the polling loop', async () => {
    mockResponseOnce(snapshot(0, 0, 0));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH });
    cell.setActive(true);
    await vi.advanceTimersByTimeAsync(0);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    cell.dispose();
    await vi.advanceTimersByTimeAsync(60_000);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it('setActive(false) stops polling without disposing the cell', async () => {
    mockResponseOnce(snapshot(0, 0, 0));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH });
    cell.setActive(true);
    await vi.advanceTimersByTimeAsync(0);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    cell.setActive(false);
    await vi.advanceTimersByTimeAsync(60_000);
    expect(fetchMock).toHaveBeenCalledTimes(1);   // no further fetches while inactive
    cell.dispose();
  });
});
