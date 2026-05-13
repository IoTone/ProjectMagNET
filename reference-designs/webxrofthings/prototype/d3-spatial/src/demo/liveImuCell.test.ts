import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import * as THREE from 'three';
import { buildLiveImuCell, type ImuSnapshot } from './liveImuCell';

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
    const airplane = cell.group.children.find(c => c.name === 'airplane-mesh');
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
    await vi.advanceTimersByTimeAsync(0);             // first response

    const airplane = cell.group.children.find(c => c.name === 'airplane-mesh')!;
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
    await vi.advanceTimersByTimeAsync(0);

    const airplane = cell.group.children.find(c => c.name === 'airplane-mesh')!;
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
    await vi.advanceTimersByTimeAsync(0);

    const airplane = cell.group.children.find(c => c.name === 'airplane-mesh')!;
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
    expect(cell.getStatus().state).toBe('live');   // initial, before any fetch
    await vi.advanceTimersByTimeAsync(0);
    expect(cell.getStatus().state).toBe('live');
    cell.dispose();
  });

  it('reports offline after two consecutive errors with no success', async () => {
    mockRejectOnce(new Error('boom'));
    mockRejectOnce(new Error('boom'));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH });
    await vi.advanceTimersByTimeAsync(0);             // err 1
    expect(cell.getStatus().state).toBe('stale');
    await vi.advanceTimersByTimeAsync(REFRESH);       // err 2 → offline
    expect(cell.getStatus().state).toBe('offline');
    cell.dispose();
  });

  it('dispose() stops the polling loop', async () => {
    mockResponseOnce(snapshot(0, 0, 0));
    const cell = buildLiveImuCell({ url: URL, refreshMs: REFRESH });
    await vi.advanceTimersByTimeAsync(0);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    cell.dispose();
    await vi.advanceTimersByTimeAsync(60_000);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });
});
