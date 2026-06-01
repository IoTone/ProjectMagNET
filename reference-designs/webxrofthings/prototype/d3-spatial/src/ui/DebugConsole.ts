/**
 * DebugConsole — a world-anchored in-scene log panel.
 *
 * Built for headsets where you can't attach `chrome://inspect` (Snap
 * Spectacles). It patches `console.*`, captures `window.onerror` +
 * `unhandledrejection`, and paints the last N lines onto a panel.
 *
 * Design history: this was originally a camera-locked HUD. That turned
 * out to be the wrong call — a panel pinned to the viewport is always
 * somewhere in the forward field, so it kept intercepting controller
 * rays meant for the sign-in panel (the XRRig draws its reticle by
 * raycasting the whole scene). Flagging it non-raycastable wasn't
 * enough because, with depthTest off, it still painted *over* the
 * interactive UI.
 *
 * Now it's a plain three-mesh-ui panel, **placed once** (on creation
 * with the desktop camera, re-placed on XR session start with the XR
 * camera so it survives Spectacles' head-relative reference space) and
 * then left world-fixed, low and forward — below the eye-level
 * interaction band so it can't sit between the user and the panels
 * they're aiming at. Normal depth testing, so closer geometry occludes
 * it instead of it painting over everything. You glance down to read
 * it; it never tracks your head.
 *
 * Gated by the caller (`?debug=1` default; `?debug=0` off).
 */

import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';

export interface DebugConsole {
  group: THREE.Group;
  /**
   * Position the panel once relative to `cam`: forward + low, then leave
   * it world-fixed (NOT camera-tracked). Call on creation (desktop cam)
   * and again on XR session start (XR cam) so the panel lands correctly
   * despite Spectacles' head-relative reference space.
   */
  place(cam: THREE.Camera): void;
  /** Per-frame: FPS accounting + throttled repaint. No repositioning. */
  tick(dtSeconds: number): void;
  /** Manually push a line (lifecycle breadcrumbs). */
  log(line: string, level?: 'log' | 'warn' | 'error'): void;
  dispose(): void;
}

const MAX_LINES = 14;
const MAX_LINE_LEN = 80;

// Panel box. A compact world panel — not a full-FOV HUD.
const PANEL_W = 0.50;
const PANEL_H = 0.30;
const PANEL_PAD = 0.015;

// Placement relative to the camera at place() time, then world-fixed:
//   - forward PLACE_DISTANCE on the horizontal heading (head pitch
//     ignored so it stays level)
//   - PLACE_DROP below the eye line — well under the sign-in panel /
//     dataspace controls so it's never in the ray path to them
const PLACE_DISTANCE = 1.5;
const PLACE_DROP = 0.55;

/**
 * High-frequency, low-signal prefixes. Matching lines still pass through
 * to the real console (desktop `[perf]` analysis unaffected) but are kept
 * OUT of the in-scene ring buffer so a real error doesn't scroll away
 * under telemetry. FPS is already shown in the panel header.
 */
const NOISE_PREFIXES = ['[perf]'];

export function createDebugConsole(): DebugConsole {
  const group = new THREE.Group();
  group.name = 'debug-console';

  // Background — a regular three-mesh-ui Block, same family as every
  // other panel in the app (join panel, dataspace menu, etc.). Normal
  // depth testing: closer geometry occludes it rather than it painting
  // over the interactive UI.
  const bgBlock = new ThreeMeshUI.Block({
    width: PANEL_W,
    height: PANEL_H,
    backgroundOpacity: 0.86,
    backgroundColor: new THREE.Color(0x05080d),
    borderRadius: 0.008,
    borderWidth: 0.0015,
    borderColor: new THREE.Color(0x2a3d52),
    borderOpacity: 0.9,
  } as any);
  bgBlock.position.set(0, 0, 0);
  group.add(bgBlock);

  // Text origins derived from the panel box so a size change can't
  // desync the layout from the background.
  const textX = -PANEL_W / 2 + PANEL_PAD;
  const headerY = PANEL_H / 2 - PANEL_PAD;

  const header = new Text();
  header.text = 'DEBUG · booting…';
  header.fontSize = 0.016;
  header.color = 0x6cf0c2;
  header.anchorX = 'left';
  header.anchorY = 'top';
  header.position.set(textX, headerY, 0.004);
  header.sync();
  group.add(header);

  const body = new Text();
  body.text = '';
  body.fontSize = 0.012;
  body.color = 0xd0d8e0;
  body.anchorX = 'left';
  body.anchorY = 'top';
  body.position.set(textX, headerY - 0.028, 0.004);
  body.maxWidth = PANEL_W - PANEL_PAD * 2;
  body.sync();
  group.add(body);

  // Defence-in-depth: a debug panel is never a pointer target. Even
  // world-anchored and out of the band, hard-disable raycast so it can
  // never be picked or block the rig reticle if the user wanders near
  // it. (The real fix is the placement above; this just makes it
  // impossible to regress by repositioning.)
  group.traverse((o) => {
    o.userData.noHover = true;
    (o as THREE.Object3D & { raycast: () => void }).raycast = () => {};
  });

  const lines: Array<{ text: string; level: 'log' | 'warn' | 'error' }> = [];
  let dirty = true;

  function pushLine(level: 'log' | 'warn' | 'error', args: unknown[]) {
    let s = args
      .map(a => {
        if (typeof a === 'string') return a;
        if (a instanceof Error) return `${a.name}: ${a.message}`;
        try { return JSON.stringify(a); } catch { return String(a); }
      })
      .join(' ');
    if (level !== 'error' && NOISE_PREFIXES.some(p => s.startsWith(p))) return;
    if (s.length > MAX_LINE_LEN) s = s.slice(0, MAX_LINE_LEN) + '…';
    lines.push({ text: s, level });
    while (lines.length > MAX_LINES) lines.shift();
    dirty = true;
  }

  // ─── Patch console + global error handlers ─────────────────────────
  const orig = {
    log: console.log.bind(console),
    warn: console.warn.bind(console),
    error: console.error.bind(console),
    info: console.info.bind(console),
  };
  console.log  = (...a: unknown[]) => { pushLine('log',  a); orig.log(...a); };
  console.info = (...a: unknown[]) => { pushLine('log',  a); orig.info(...a); };
  console.warn = (...a: unknown[]) => { pushLine('warn', a); orig.warn(...a); };
  console.error= (...a: unknown[]) => { pushLine('error',a); orig.error(...a); };

  const onError = (e: ErrorEvent) => {
    pushLine('error', [`window.onerror: ${e.message} @ ${e.filename}:${e.lineno}`]);
  };
  const onRejection = (e: PromiseRejectionEvent) => {
    const r = e.reason;
    pushLine('error', [`unhandledrejection: ${r instanceof Error ? r.message : String(r)}`]);
  };
  if (typeof window !== 'undefined') {
    window.addEventListener('error', onError);
    window.addEventListener('unhandledrejection', onRejection);
  }

  // ─── FPS (rolling average over ~1 s) ───────────────────────────────
  let fpsAccum = 0;
  let fpsFrames = 0;
  let fps = 0;
  let repaintAccum = 0;

  // Scratch for place().
  const camPos = new THREE.Vector3();
  const camQuat = new THREE.Quaternion();
  const fwd = new THREE.Vector3();

  function repaint() {
    header.text = `DEBUG · ${fps.toFixed(0)} fps · ${lines.length} msgs`;
    const hasError = lines.some(l => l.level === 'error');
    const hasWarn = lines.some(l => l.level === 'warn');
    header.color = hasError ? 0xff6b6b : hasWarn ? 0xf0c674 : 0x6cf0c2;
    header.sync();
    body.text = lines.map(l => {
      const tag = l.level === 'error' ? '✖ ' : l.level === 'warn' ? '▲ ' : '· ';
      return tag + l.text;
    }).join('\n');
    body.sync();
  }

  return {
    group,
    place(cam: THREE.Camera) {
      cam.getWorldPosition(camPos);
      cam.getWorldQuaternion(camQuat);
      fwd.set(0, 0, -1).applyQuaternion(camQuat);
      fwd.y = 0;                                  // keep level, ignore head pitch
      if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
      fwd.normalize();
      group.position.set(
        camPos.x + fwd.x * PLACE_DISTANCE,
        camPos.y - PLACE_DROP,
        camPos.z + fwd.z * PLACE_DISTANCE,
      );
      // Face the user, upright (look at a point at the panel's own
      // height so it isn't tilted up at the face).
      group.lookAt(camPos.x, group.position.y, camPos.z);
      dirty = true;
    },
    tick(dtSeconds: number) {
      fpsAccum += dtSeconds;
      fpsFrames += 1;
      if (fpsAccum >= 1) {
        fps = fpsFrames / fpsAccum;
        fpsAccum = 0;
        fpsFrames = 0;
        dirty = true;
      }
      repaintAccum += dtSeconds;
      if (dirty && repaintAccum > 0.15) {
        repaintAccum = 0;
        dirty = false;
        repaint();
      }
    },
    log(line: string, level: 'log' | 'warn' | 'error' = 'log') {
      pushLine(level, [line]);
    },
    dispose() {
      console.log = orig.log;
      console.warn = orig.warn;
      console.error = orig.error;
      console.info = orig.info;
      if (typeof window !== 'undefined') {
        window.removeEventListener('error', onError);
        window.removeEventListener('unhandledrejection', onRejection);
      }
      header.dispose();
      body.dispose();
    },
  };
}
