/**
 * Live IMU cell — polls an IMU snapshot endpoint and rotates a small
 * airplane mesh by the latest Euler reading.
 *
 * Today the data comes from the mock-join-server's simulated feed
 * (`/api/v1/sensor/imu`) for UC4. The real-device swap is meant to be a
 * single manifest URL change — the response shape (Euler radians +
 * accel + angular velocity, matching consumer IMU conventions) is what
 * a real device would emit too, so this module has no sim-specific code.
 *
 * Reuses `startPolling` from `livePolling` so the IMU gets the same
 * offline/stale/live status hysteresis as the vitals cells. The status
 * badge wiring in the gallery wrapper will Just Work if/when this cell
 * is shown alongside a status badge.
 */
import * as THREE from 'three';
import { startPolling } from './livePolling';
import type { LiveCellStatus } from './livePolling';
import { TEXT } from '../ui/palette';

/** Shape of the JSON the IMU endpoint returns. Both `/api/v1/sensor/imu`
 *  in the mock server and any future real device must match this. */
export interface ImuSnapshot {
  orientation: { roll_rad: number; pitch_rad: number; yaw_rad: number };
  angular_velocity: { x: number; y: number; z: number };
  acceleration:     { x: number; y: number; z: number };
  timestamp_us?: number;
}

export interface LiveImuOpts {
  url: string;
  refreshMs?: number;
  /** Half-length of the airplane fuselage in world units. Default 0.10. */
  size?: number;
  /** Body color (hex). Default: warm primary palette. */
  color?: number;
  /**
   * If true, slerp toward the new orientation in `tick()` for smooth
   * motion between fetches. If false, snap on each refresh. Default true.
   */
  smooth?: boolean;
}

export interface LiveImuCell {
  group: THREE.Group;
  tick(time: number): void;
  dispose(): void;
  getStatus(): LiveCellStatus;
}

/**
 * Build a small wireframe airplane that orients itself by the IMU stream.
 * The airplane is composed entirely of cheap THREE primitives (no models
 * to load) so it works identically on every platform we ship to.
 *
 * Coordinate convention: airplane noses forward along +Z (matches the
 * rest of the project's "forward = -Z when looking" inversion handled
 * by the parent group's placement). Roll = rotation around +Z (bank),
 * pitch = rotation around +X (nose up/down), yaw = rotation around +Y.
 */
export function buildLiveImuCell(opts: LiveImuOpts): LiveImuCell {
  const {
    url, refreshMs = 200, size = 0.10, color = TEXT.primary, smooth = true,
  } = opts;

  const group = new THREE.Group();
  group.name = `live-imu:${url}`;

  // Inner group carries the airplane mesh; rotating the inner lets us
  // keep `group` available for placement (translation) by callers.
  const airplane = new THREE.Group();
  airplane.name = 'airplane-mesh';
  group.add(airplane);

  const mat = new THREE.MeshBasicMaterial({ color, wireframe: false });
  // Fuselage — long thin box along Z.
  const fuselage = new THREE.Mesh(
    new THREE.BoxGeometry(size * 0.16, size * 0.16, size * 1.4),
    mat,
  );
  airplane.add(fuselage);
  // Wings — wider, very thin box, perpendicular to fuselage.
  const wings = new THREE.Mesh(
    new THREE.BoxGeometry(size * 1.6, size * 0.04, size * 0.32),
    mat,
  );
  wings.position.set(0, 0, 0);
  airplane.add(wings);
  // Tail fin — small vertical box near the rear.
  const tail = new THREE.Mesh(
    new THREE.BoxGeometry(size * 0.04, size * 0.28, size * 0.18),
    mat,
  );
  tail.position.set(0, size * 0.16, -size * 0.55);
  airplane.add(tail);
  // Tail stabilizer — horizontal at the rear.
  const stab = new THREE.Mesh(
    new THREE.BoxGeometry(size * 0.5, size * 0.04, size * 0.12),
    mat,
  );
  stab.position.set(0, 0, -size * 0.6);
  airplane.add(stab);

  // Latest target orientation as a quaternion. Updated on refresh; the
  // tick() loop slerps `airplane.quaternion` toward it for smooth motion.
  const targetQuat = new THREE.Quaternion();
  // Slerp rate per tick (60Hz tick × 0.12 ≈ ~80 ms half-life — fast enough
  // to track real IMU at 10–50 Hz, slow enough to hide sample jitter).
  const SLERP_RATE = 0.12;

  // Scratch Euler so we don't allocate per tick.
  const scratchEuler = new THREE.Euler(0, 0, 0, 'YXZ');
  const scratchQuat = new THREE.Quaternion();

  async function refresh(): Promise<boolean> {
    try {
      const resp = await fetch(url);
      if (!resp.ok) return false;
      const json = await resp.json() as Partial<ImuSnapshot>;
      const o = json?.orientation;
      if (!o || typeof o.roll_rad !== 'number'
            || typeof o.pitch_rad !== 'number'
            || typeof o.yaw_rad !== 'number') {
        // Empty / unexpected shape — treat as success (poll continues) but
        // don't move the airplane. Matches the "no data yet" behaviour in
        // the vitals cells.
        return true;
      }
      // YXZ Euler order: yaw applied first (heading), then pitch (nose),
      // then roll (bank). Matches the way pilots think about attitude.
      scratchEuler.set(o.pitch_rad, o.yaw_rad, o.roll_rad, 'YXZ');
      scratchQuat.setFromEuler(scratchEuler);
      targetQuat.copy(scratchQuat);
      if (!smooth) airplane.quaternion.copy(targetQuat);
      return true;
    } catch {
      /* leave airplane at last known orientation */
      return false;
    }
  }

  const poller = startPolling(refreshMs, refresh);

  // Self-driven slerp via three.js's per-render hook. Fires on every frame
  // the airplane is actually rendered (skips when culled) and doesn't need
  // the manifest loader to plumb a per-frame tick — important because the
  // manifest pipeline currently doesn't have one. The existing `tick()`
  // method on this interface is kept for parity with the vitals cells
  // but isn't load-bearing.
  fuselage.onBeforeRender = () => {
    if (smooth) airplane.quaternion.slerp(targetQuat, SLERP_RATE);
  };

  return {
    group,
    tick: (_time: number) => { /* no-op — slerp lives in onBeforeRender */ },
    dispose: () => {
      poller.stop();
      fuselage.onBeforeRender = () => {};
      fuselage.geometry.dispose();
      wings.geometry.dispose();
      tail.geometry.dispose();
      stab.geometry.dispose();
      mat.dispose();
    },
    getStatus: () => poller.getStatus(),
  };
}
