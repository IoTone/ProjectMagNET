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
import { Text } from 'troika-three-text';
import { startPolling } from './livePolling';
import type { LiveCellStatus, PollingHandle } from './livePolling';
import type { GlobeWidget } from './globeWidget';
import { TEXT } from '../ui/palette';

/**
 * Hard-disable the three-globe widget while UC4 perf is being debugged.
 *
 * Symptoms that triggered this gate (2026-05-14):
 *   - Snap Spectacles: scene hangs on UC4 entry.
 *   - Quest 3: renders occasionally, then freezes and unfreezes in cycles.
 *   - Desktop browser: looks fine.
 *
 * The desktop-only-OK pattern points at the three-globe chunk being too
 * heavy for the mobile XR GPUs (atmosphere ShaderMaterial + full-mesh
 * SphereGeometry + texture upload + animation tween). Pulling it out of
 * the cell entirely so we can isolate whether it's the perf culprit, vs.
 * a separate cell-positioning issue ("items should be fixed in space,
 * not tied to the camera") that needs its own investigation.
 *
 * To re-enable: flip this constant to `false`. The lazy `import()` only
 * fires when the constant is false, so the entire ~475 KB globeWidget
 * chunk stays unloaded while the gate is set.
 */
const GLOBE_DISABLED_PENDING_PERF_DEBUG = false;

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
  /**
   * If true (default), embed a small slowly-rotating three-globe behind the
   * airplane, textured with the locally-cached OSM zoom-0 world tile. The
   * globe is presentation-only — no flight-tracking math behind it.
   */
  showGlobe?: boolean;
}

export interface LiveImuCell {
  group: THREE.Group;
  tick(time: number): void;
  /**
   * Toggle whether the cell is allowed to do background work. When false
   * the IMU polling loop is stopped (no fetches to /api/v1/sensor/imu);
   * when true a fresh poller is started so the airplane resumes tracking
   * the live stream. Constructor leaves the cell inactive — the
   * orchestrator calls setActive(true) when this mode is on-stage.
   */
  setActive(active: boolean): void;
  dispose(): void;
  getStatus(): LiveCellStatus;
}

/** Module-level instance counter — same idea as liveSplatGalleryCell: a
 *  mismatch between "constructed" and "disposed" log counts surfaces
 *  zombie cells from HMR or leaked dispose chains. */
let nextImuCellId = 1;

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
    showGlobe = true,
  } = opts;

  const group = new THREE.Group();
  group.name = `live-imu:${url}`;

  // ─── Layout ────────────────────────────────────────────────────────
  //
  // The cell splits horizontally: the airplane + its six instrument
  // labels live in `panel`, offset to the left; the globe (added later
  // by the dynamic `globeWidget` import) sits on the right.
  //
  // Originally the airplane and globe shared the cell centre, with the
  // globe scaled large enough to use as a backdrop — that ended up with
  // the airplane sitting directly on top of the globe, instrument
  // labels overlapping the continents. Side-by-side reads cleaner and
  // lets us shrink the globe back to a sensible standalone size.
  const PANEL_X = -size * 1.5;      // airplane + instruments offset
  const GLOBE_X =  size * 1.8;      // globe offset
  const GLOBE_WORLD_RADIUS = size * 1.1;

  const panel = new THREE.Group();
  panel.name = 'instrument-panel';
  panel.position.x = PANEL_X;
  group.add(panel);

  // Inner group carries the airplane mesh; rotating the inner lets us
  // keep `panel` available for translation by callers.
  const airplane = new THREE.Group();
  airplane.name = 'airplane-mesh';
  panel.add(airplane);

  // Wireframe rendering so the airplane reads as a synthwave HUD prop rather
  // than a solid plane — matches the boombox + globe atmosphere and lets the
  // globe behind it stay visible through the fuselage.
  const mat = new THREE.MeshBasicMaterial({ color, wireframe: true });
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

  // ─── Instrument panel — heading + airspeed ────────────────────────
  //
  // Inspired by the Garmin G5 EFI layout: a fake compass heading (degrees
  // magnetic) on the right and a fake airspeed indicator (KIAS) on the
  // left, mirrored across the airplane in the middle. Both sit at the
  // airplane's vertical centre so they don't collide with the
  // renderManifest-injected mark title above the cell or the subtitle
  // below it — the earlier "HDG above" layout overlapped the cell title.
  //
  // Heading derives from yaw on the IMU stream so it agrees with the
  // airplane's apparent rotation; airspeed is a slow sine-driven number
  // to look "in flight."
  const airspeedLabel = new Text();
  airspeedLabel.text = '--- KT';
  airspeedLabel.fontSize = size * 0.18;
  airspeedLabel.color = TEXT.body;
  airspeedLabel.anchorX = 'right';
  airspeedLabel.anchorY = 'middle';
  airspeedLabel.position.set(-size * 1.3, 0, 0);
  airspeedLabel.sync();
  panel.add(airspeedLabel);

  const airspeedSubLabel = new Text();
  airspeedSubLabel.text = 'AIRSPEED · KIAS';
  airspeedSubLabel.fontSize = size * 0.06;
  airspeedSubLabel.color = TEXT.muted;
  airspeedSubLabel.anchorX = 'right';
  airspeedSubLabel.anchorY = 'middle';
  airspeedSubLabel.position.set(-size * 1.3, -size * 0.18, 0);
  airspeedSubLabel.sync();
  panel.add(airspeedSubLabel);

  const headingLabel = new Text();
  headingLabel.text = 'HDG ---°';
  headingLabel.fontSize = size * 0.18;
  headingLabel.color = TEXT.body;
  headingLabel.anchorX = 'left';
  headingLabel.anchorY = 'middle';
  headingLabel.position.set(size * 1.3, 0, 0);
  headingLabel.sync();
  panel.add(headingLabel);

  const headingSubLabel = new Text();
  headingSubLabel.text = 'HEADING · MAG';
  headingSubLabel.fontSize = size * 0.06;
  headingSubLabel.color = TEXT.muted;
  headingSubLabel.anchorX = 'left';
  headingSubLabel.anchorY = 'middle';
  headingSubLabel.position.set(size * 1.3, -size * 0.18, 0);
  headingSubLabel.sync();
  panel.add(headingSubLabel);

  // Altitude readout sits below the airplane on centre — the IMU stream
  // doesn't carry barometric altitude (it's a pressure-sensor reading,
  // not an inertial one), so this is synthesised client-side. Cruise
  // altitudes for commercial jets are in the 30,000–40,000 ft band.
  const altitudeLabel = new Text();
  altitudeLabel.text = '----- FT';
  altitudeLabel.fontSize = size * 0.18;
  altitudeLabel.color = TEXT.body;
  altitudeLabel.anchorX = 'center';
  altitudeLabel.anchorY = 'middle';
  altitudeLabel.position.set(0, -size * 0.55, 0);
  altitudeLabel.sync();
  panel.add(altitudeLabel);

  const altitudeSubLabel = new Text();
  altitudeSubLabel.text = 'ALTITUDE · MSL';
  altitudeSubLabel.fontSize = size * 0.06;
  altitudeSubLabel.color = TEXT.muted;
  altitudeSubLabel.anchorX = 'center';
  altitudeSubLabel.anchorY = 'middle';
  altitudeSubLabel.position.set(0, -size * 0.73, 0);
  altitudeSubLabel.sync();
  panel.add(altitudeSubLabel);

  // Cruise around 350 KIAS with small ±15 KT variation — feels alive but
  // doesn't suggest aggressive maneuvering.
  function airspeedAt(t: number): number {
    const m = t / 1000;
    return 350 + 15 * Math.sin(m / 17) + 4 * Math.sin(m / 3.2);
  }

  // Cruise around 35,000 ft with slow ±1,200 ft variation. Rounded to the
  // nearest 100 ft so the display ticks visibly rather than flickering.
  function altitudeAt(t: number): number {
    const m = t / 1000;
    return 35000 + 1200 * Math.sin(m / 23) + 180 * Math.sin(m / 5.1);
  }

  // ─── Globe (three-globe) ──────────────────────────────────────────────
  //
  // Slowly-rotating Earth behind the airplane, lazily imported so the
  // three-globe dep chain (which touches `window` at module load) is
  // only evaluated in the browser. Node-based vitest runners skip this
  // path entirely and don't need to mock three-globe.
  //
  // The globe wrapper is fire-and-forget: construction stays synchronous,
  // the cell renders without the globe initially, and the globe attaches
  // itself to `group` once the dynamic import resolves. If the cell is
  // disposed before the import finishes (e.g., user leaves UC4 fast),
  // `disposed` guards prevent attaching a now-orphaned globe.
  let globeWidget: GlobeWidget | null = null;
  let disposed = false;
  if (GLOBE_DISABLED_PENDING_PERF_DEBUG) {
    // Loud one-line tag so it's visible in headset webview logs without
    // hunting. Anyone debugging the empty right-side of the cell can
    // grep this string to find the gate.
    console.warn(
      '[liveImuCell] globeWidget DISABLED (GLOBE_DISABLED_PENDING_PERF_DEBUG=true) — '
        + 'flip the flag in src/demo/liveImuCell.ts to re-enable. Held off to '
        + 'isolate Snap Spectacles hang / Quest 3 freeze-unfreeze cycles in UC4.',
    );
  } else if (showGlobe && typeof window !== 'undefined') {
    console.info('[liveImuCell] requesting globeWidget…');
    void import('./globeWidget').then(({ buildGlobeWidget }) => {
      if (disposed) {
        console.info('[liveImuCell] cell disposed before globe loaded — skipping');
        return;
      }
      // Globe sits on the right side of the cell, mirroring the airplane
      // + instrument panel on the left. Sized to roughly match the
      // airplane's wing-span so neither dominates the other.
      const w = buildGlobeWidget({ worldRadius: GLOBE_WORLD_RADIUS });
      w.object.position.set(GLOBE_X, 0, 0);
      group.add(w.object);
      globeWidget = w;
      console.info('[liveImuCell] globe attached', w.object.position);
    }).catch((err) => {
      // three-globe failed to import (e.g., bad network on a CDN-served
      // build, or a peer-dep mismatch surfacing at runtime). The rest of
      // the cell is still useful — log and continue without the globe.
      console.warn('[liveImuCell] globe widget failed to load:', err);
    });
  }

  // Latest target orientation as a quaternion. Updated on refresh; the
  // tick() loop slerps `airplane.quaternion` toward it for smooth motion.
  const targetQuat = new THREE.Quaternion();
  // Latest yaw for heading display.
  let currentYawRad = 0;
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
      currentYawRad = o.yaw_rad;
      if (!smooth) airplane.quaternion.copy(targetQuat);
      // Update the instrument labels from the same stream. Heading is the
      // yaw mapped to 0–359° magnetic; airspeed is the time-based fake.
      const hdgDeg = Math.round(((currentYawRad * 180 / Math.PI) % 360 + 360) % 360);
      headingLabel.text = `HDG ${hdgDeg.toString().padStart(3, '0')}°`;
      headingLabel.sync();
      airspeedLabel.text = `${Math.round(airspeedAt(Date.now()))} KT`;
      airspeedLabel.sync();
      const altFt = Math.round(altitudeAt(Date.now()) / 100) * 100;
      altitudeLabel.text = `${altFt.toLocaleString()} FT`;
      altitudeLabel.sync();
      return true;
    } catch {
      /* leave airplane at last known orientation */
      return false;
    }
  }

  const cellId = nextImuCellId++;
  console.info(`[liveImuCell #${cellId}] constructed (url=${url}, refreshMs=${refreshMs})`);

  // Poller is owned lazily — constructor doesn't poll. setActive(true)
  // starts a fresh poller, setActive(false) stops it. The airplane keeps
  // rendering at its last-known orientation while inactive (and three.js
  // skips drawing it altogether when its enclosing cell is hidden).
  let poller: PollingHandle | null = null;
  let active = false;

  function setActive(next: boolean) {
    if (next === active) return;
    active = next;
    console.info(`[liveImuCell #${cellId}] setActive(${active})`);
    if (active) {
      poller = startPolling(refreshMs, refresh);
    } else {
      poller?.stop();
      poller = null;
    }
  }

  // Self-driven slerp via three.js's per-render hook. Fires on every frame
  // the airplane is actually rendered (skips when culled) and doesn't need
  // the manifest loader to plumb a per-frame tick — important because the
  // manifest pipeline currently doesn't have one. The existing `tick()`
  // method on this interface is kept for parity with the vitals cells
  // but isn't load-bearing.
  fuselage.onBeforeRender = () => {
    if (smooth) airplane.quaternion.slerp(targetQuat, SLERP_RATE);
    // Slow eastward globe rotation, applied via the widget so the
    // rotation-per-frame stays colocated with the globe construction.
    globeWidget?.update();
  };

  return {
    group,
    tick: (_time: number) => { /* no-op — slerp lives in onBeforeRender */ },
    setActive,
    dispose: () => {
      console.info(`[liveImuCell #${cellId}] disposed`);
      active = false;
      poller?.stop();
      poller = null;
      fuselage.onBeforeRender = () => {};
      fuselage.geometry.dispose();
      wings.geometry.dispose();
      tail.geometry.dispose();
      stab.geometry.dispose();
      mat.dispose();
      headingLabel.dispose();
      headingSubLabel.dispose();
      airspeedLabel.dispose();
      airspeedSubLabel.dispose();
      altitudeLabel.dispose();
      altitudeSubLabel.dispose();
      // If the globe import is still in-flight, this flag prevents the
      // .then handler from attaching an orphan widget to a disposed group.
      disposed = true;
      globeWidget?.dispose();
    },
    getStatus: () => poller?.getStatus() ?? { state: 'offline' as const, lastSuccessAgoMs: null },
  };
}
