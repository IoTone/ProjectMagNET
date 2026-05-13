/**
 * Mock Join Server — P1.1
 *
 * Generates rotating 6-char join codes and validates them via POST /api/v1/join.
 * Serves the room-dataspace manifest via GET /api/v1/manifest (JWT-protected).
 * Exposes GET /api/v1/code for development convenience.
 */

import express, { type Express } from 'express';
import jwt from 'jsonwebtoken';
import { readFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import crypto from 'crypto';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Full A-Z + 0-9 (36 chars). Originally ambiguity-stripped to A-Z minus
// O/I/L plus digits 2-9 (no 0/1) for cleaner code handoff, but that made
// the fixed UC codes (DEMO01-04) untypable in the slot wheel because they
// contain O, 0, and 1. Reverting to the full alphanumeric set so the demo
// codes work; rotating-code readability takes a minor hit.
export const CHAR_SET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
const CODE_LENGTH = 6;

/**
 * Hardcoded "demo" codes that always resolve to a specific use-case manifest.
 * Coexist with the rotating dev code — fixed codes are checked first, the
 * rotating code is checked second. Each fixed code corresponds to one
 * dataspace, so the manifest path can be selected per-code instead of being
 * a server-wide default.
 *
 * Paths are resolved relative to the `examples/` directory at server-create
 * time. Override via the `fixedCodes` option for tests.
 */
export const DEFAULT_FIXED_CODES: Record<string, string> = {
  DEMO01: 'uc1-vitals.json',
  DEMO02: 'uc2-room.json',
  DEMO03: 'uc3-poster.json',
  DEMO04: 'uc4-airplane.json',
};

export interface JoinServerOptions {
  /** Default 'hlxr-dev-secret'. */
  jwtSecret?: string;
  /** Default 300 (5 min). 0 disables rotation timer. */
  rotationSeconds?: number;
  /** Default manifest for the rotating dev code. examples/room-dataspace.json by default. */
  manifestPath?: string;
  /**
   * Map of fixed code → absolute manifest path. Defaults to DEFAULT_FIXED_CODES
   * resolved against `examples/`. Pass a custom map (with absolute paths) in tests.
   */
  fixedCodes?: Record<string, string>;
  /** Per-IP rate limit per 60s window. Default 5. */
  rateLimitPerMinute?: number;
  /** Whether to start the rotation interval timer. Default true. */
  startRotationTimer?: boolean;
}

export interface JoinServer {
  app: Express;
  getCurrentCode: () => string;
  getPreviousCode: () => string | null;
  rotateCode: () => void;
  resetCounter: () => void;
  stopRotationTimer: () => void;
}

/** Build a fresh join-server instance. Each call has independent state — safe for tests. */
export function createJoinServer(opts: JoinServerOptions = {}): JoinServer {
  const JWT_SECRET = opts.jwtSecret ?? 'hlxr-dev-secret';
  const ROTATION_SECONDS = opts.rotationSeconds ?? 300;
  const RATE_LIMIT = opts.rateLimitPerMinute ?? 5;
  const examplesDir = join(__dirname, '..', 'examples');
  const manifestPath = opts.manifestPath ?? join(examplesDir, 'room-dataspace.json');
  // Resolve fixed-code paths against examples/ when caller didn't provide
  // absolute paths. Tests pass absolute paths to point at fixtures.
  const fixedCodes: Record<string, string> = (() => {
    const raw = opts.fixedCodes ?? DEFAULT_FIXED_CODES;
    const resolved: Record<string, string> = {};
    for (const [code, p] of Object.entries(raw)) {
      resolved[code.toUpperCase()] = p.startsWith('/') ? p : join(examplesDir, p);
    }
    return resolved;
  })();

  const app = express();
  app.use(express.json());

  let codeCounter = 0;
  function generateCode(): string {
    const base = CHAR_SET.length;
    let n = codeCounter++;
    let code = '';
    for (let i = 0; i < CODE_LENGTH; i++) {
      code = CHAR_SET[n % base] + code;
      n = Math.floor(n / base);
    }
    return code;
  }

  let currentCode = generateCode();
  let previousCode: string | null = null;
  const expiredCodes: Set<string> = new Set();
  let codeGeneratedAt = Date.now();

  function rotateCode() {
    if (previousCode) {
      expiredCodes.add(previousCode);
      if (expiredCodes.size > 10) {
        const first = expiredCodes.values().next().value;
        if (first !== undefined) expiredCodes.delete(first);
      }
    }
    previousCode = currentCode;
    currentCode = generateCode();
    codeGeneratedAt = Date.now();
  }

  let rotationTimer: NodeJS.Timeout | null = null;
  if ((opts.startRotationTimer ?? true) && ROTATION_SECONDS > 0) {
    rotationTimer = setInterval(rotateCode, ROTATION_SECONDS * 1000);
  }

  const rateLimits = new Map<string, { count: number; windowStart: number }>();
  function isRateLimited(ip: string): boolean {
    const now = Date.now();
    const entry = rateLimits.get(ip);
    if (!entry || now - entry.windowStart > 60_000) {
      rateLimits.set(ip, { count: 1, windowStart: now });
      return false;
    }
    entry.count++;
    return entry.count > RATE_LIMIT;
  }

  app.post('/api/v1/join', (req, res) => {
    const ip = req.ip ?? req.socket.remoteAddress ?? 'unknown';

    if (isRateLimited(ip)) {
      res.status(429).json({ status: 'rejected', reason: 'rate_limited' });
      return;
    }

    const { code } = req.body as { code?: string };
    if (!code || typeof code !== 'string') {
      res.status(400).json({ status: 'rejected', reason: 'invalid' });
      return;
    }

    const upper = code.toUpperCase();

    // Fixed codes win over the rotating code so a tester typing DEMO03 always
    // gets UC3, even on the rare case the rotating generator happens to hit
    // the same letters. (CHAR_SET excludes 0/1 so DEMO0X can't actually be
    // rotated into, but we belt-and-suspenders the order anyway.)
    if (upper in fixedCodes) {
      const dataspace = upper.toLowerCase();
      const session = crypto.randomUUID();
      const token = jwt.sign(
        { dataspace, session, manifest_path: fixedCodes[upper], iat: Math.floor(Date.now() / 1000) },
        JWT_SECRET,
        { expiresIn: '1h' },
      );
      res.json({
        status: 'accepted',
        token,
        manifest_url: '/api/v1/manifest',
        dataspace,
      });
      return;
    }

    if (upper === currentCode || upper === previousCode) {
      const session = crypto.randomUUID();
      const token = jwt.sign(
        { dataspace: 'demo-room', session, iat: Math.floor(Date.now() / 1000) },
        JWT_SECRET,
        { expiresIn: '1h' },
      );
      res.json({
        status: 'accepted',
        token,
        manifest_url: '/api/v1/manifest',
        dataspace: 'demo-room',
      });
      return;
    }

    if (expiredCodes.has(upper)) {
      res.json({ status: 'rejected', reason: 'expired' });
      return;
    }

    res.json({ status: 'rejected', reason: 'invalid' });
  });

  app.get('/api/v1/manifest', (req, res) => {
    const auth = req.headers.authorization;
    if (!auth || !auth.startsWith('Bearer ')) {
      res.status(401).json({ error: 'missing_token' });
      return;
    }

    const token = auth.slice(7);
    let payload: jwt.JwtPayload;
    try {
      payload = jwt.verify(token, JWT_SECRET) as jwt.JwtPayload;
    } catch {
      res.status(401).json({ error: 'invalid_token' });
      return;
    }

    // Per-code dispatch: tokens minted for fixed codes carry their manifest
    // path in `manifest_path`. Tokens from the rotating code fall through to
    // the server's default manifestPath, preserving prior behaviour.
    const path = typeof payload.manifest_path === 'string'
      ? payload.manifest_path
      : manifestPath;
    try {
      const manifest = JSON.parse(readFileSync(path, 'utf-8'));
      res.json(manifest);
    } catch {
      res.status(500).json({ error: 'manifest_read_failed' });
    }
  });

  app.get('/api/v1/code', (_req, res) => {
    const elapsed = (Date.now() - codeGeneratedAt) / 1000;
    const expiresIn = Math.max(0, Math.round(ROTATION_SECONDS - elapsed));
    res.json({ code: currentCode, expiresIn });
  });

  // ─── Simulated sensor feeds (P3+) ─────────────────────────────────────
  // Establishes the "fake feed" pattern used by P3 (UC1 body temperature)
  // and reused by P4 (UC2 AQI/barometer/pollen) and P5 (UC4 IMU). Each
  // endpoint returns a deterministic, time-derived signal so the same
  // request issued at the same wall-clock time yields the same samples —
  // useful for smoke baselines and visual regression.

  /**
   * GET /api/v1/sensor/body-temperature/history
   * Body temperature, last 60 minutes, 1 sample/min. Deterministic noisy
   * signal centred on 36.7 °C with a slow circadian-like wander (~±0.25 °C),
   * a faster ~5-min ripple (~±0.05 °C), and a pseudo-noise term derived from
   * the minute index. Range ~36.4-37.0 °C — comfortable normal band.
   */
  app.get('/api/v1/sensor/body-temperature/history', (_req, res) => {
    const now = Date.now();
    const samples: Array<{ t: number; v: number }> = [];
    for (let i = 59; i >= 0; i--) {
      const t = now - i * 60_000;
      const m = t / 60_000;
      const v = 36.7
        + 0.25 * Math.sin(m / 30)
        + 0.05 * Math.sin(m / 5)
        + 0.02 * Math.sin(m * 7.13);
      samples.push({ t, v: Math.round(v * 100) / 100 });
    }
    res.json({ samples });
  });

  /**
   * GET /api/v1/sensor/body-temperature
   * Snapshot of the current body temperature reading (mirrors the device
   * snapshot endpoints like /heart-rate). Pluck-friendly shape.
   */
  app.get('/api/v1/sensor/body-temperature', (_req, res) => {
    const now = Date.now();
    const m = now / 60_000;
    const celsius = 36.7
      + 0.25 * Math.sin(m / 30)
      + 0.05 * Math.sin(m / 5)
      + 0.02 * Math.sin(m * 7.13);
    res.json({
      celsius: Math.round(celsius * 100) / 100,
      timestamp_us: now * 1000,
    });
  });

  // ─── UC2 environmental sensors (P4a) — AQI / barometer / pollen ────────
  // Same deterministic-time pattern as body-temp. Each feed has a snapshot
  // endpoint (current value + categorical level) and a 60-sample history.
  // Internal generators live alongside the endpoints so tests can assert
  // value bounds without re-implementing the maths.

  function aqiAt(t: number): number {
    const m = t / 60_000;
    return 50 + 25 * Math.sin(m / 30) + 10 * Math.sin(m / 7.5);
  }
  function aqiCategory(aqi: number): string {
    if (aqi < 50) return 'good';
    if (aqi < 100) return 'moderate';
    if (aqi < 150) return 'unhealthy-for-sensitive';
    if (aqi < 200) return 'unhealthy';
    if (aqi < 300) return 'very-unhealthy';
    return 'hazardous';
  }
  app.get('/api/v1/sensor/aqi/history', (_req, res) => {
    const now = Date.now();
    const samples: Array<{ t: number; v: number }> = [];
    for (let i = 59; i >= 0; i--) {
      const t = now - i * 60_000;
      samples.push({ t, v: Math.round(aqiAt(t)) });
    }
    res.json({ samples });
  });
  app.get('/api/v1/sensor/aqi', (_req, res) => {
    const now = Date.now();
    const aqi = Math.round(aqiAt(now));
    res.json({ aqi, category: aqiCategory(aqi), timestamp_us: now * 1000 });
  });

  function hpaAt(t: number): number {
    // Atmospheric pressure: ~1009-1021 hPa range, slow weather-like drift.
    const m = t / 60_000;
    return 1015 + 5 * Math.sin(m / 60) + Math.sin(m / 7);
  }
  app.get('/api/v1/sensor/barometer/history', (_req, res) => {
    const now = Date.now();
    const samples: Array<{ t: number; v: number }> = [];
    for (let i = 59; i >= 0; i--) {
      const t = now - i * 60_000;
      samples.push({ t, v: Math.round(hpaAt(t) * 10) / 10 });
    }
    res.json({ samples });
  });
  app.get('/api/v1/sensor/barometer', (_req, res) => {
    const now = Date.now();
    const hpa = Math.round(hpaAt(now) * 10) / 10;
    // Trend over the last 30 min — useful demo signal for "weather changing".
    const earlier = hpaAt(now - 30 * 60_000);
    const trend = hpa - earlier > 0.5 ? 'rising' : hpa - earlier < -0.5 ? 'falling' : 'steady';
    res.json({ hpa, trend, timestamp_us: now * 1000 });
  });

  function pollenAt(t: number): number {
    // Daily cycle — pollen peaks mid-morning. Range ~1-7 (clipped at 0).
    const m = t / 60_000;
    return Math.max(0, 4 + 3 * Math.sin(m / 120) + 0.5 * Math.sin(m * 1.7));
  }
  function pollenLevel(count: number): string {
    if (count < 2.4) return 'low';
    if (count < 4.8) return 'moderate';
    if (count < 7.2) return 'high';
    return 'very-high';
  }
  app.get('/api/v1/sensor/pollen/history', (_req, res) => {
    const now = Date.now();
    const samples: Array<{ t: number; v: number }> = [];
    for (let i = 59; i >= 0; i--) {
      const t = now - i * 60_000;
      samples.push({ t, v: Math.round(pollenAt(t) * 10) / 10 });
    }
    res.json({ samples });
  });
  app.get('/api/v1/sensor/pollen', (_req, res) => {
    const now = Date.now();
    const count = Math.round(pollenAt(now) * 10) / 10;
    res.json({ count, level: pollenLevel(count), timestamp_us: now * 1000 });
  });

  // ─── UC2 actuators (P4b) — lighting / thermostat / speaker ─────────────
  // Stateful: GET reads current state, POST mutates. Each actuator keeps an
  // in-memory state record + an event log. The history endpoints return
  // 60-sample series suitable for line marks; samples are read straight from
  // the event log (or filled with the current value if the log is sparse).
  //
  // No persistence — server restart resets state to defaults. That's fine
  // for a demo: it's the dataspace's job, not the test fixture's.

  interface LightState {
    on: boolean;
    brightness_pct: number;     // 0-100
    color: { r: number; g: number; b: number };  // 0-255 each
    ramp_ms: number;             // transition time on change
    last_changed_at: number;     // ms epoch
  }
  const lightState: LightState = {
    on: true, brightness_pct: 70,
    color: { r: 255, g: 220, b: 180 },   // warm white default
    ramp_ms: 500,
    last_changed_at: Date.now(),
  };
  // Event log: [{t_ms, brightness_pct, on}] for the brightness history line.
  const lightHistory: Array<{ t: number; v: number }> = [];

  app.get('/api/v1/actuator/light', (_req, res) => {
    res.json({ ...lightState, timestamp_us: Date.now() * 1000 });
  });
  app.post('/api/v1/actuator/light', (req, res) => {
    const body = req.body as Partial<LightState> & { on?: boolean };
    if (typeof body.on === 'boolean') lightState.on = body.on;
    if (typeof body.brightness_pct === 'number') {
      lightState.brightness_pct = Math.max(0, Math.min(100, body.brightness_pct));
    }
    if (body.color && typeof body.color === 'object') {
      const { r, g, b } = body.color;
      if ([r, g, b].every(c => typeof c === 'number')) {
        lightState.color = {
          r: Math.max(0, Math.min(255, r)),
          g: Math.max(0, Math.min(255, g)),
          b: Math.max(0, Math.min(255, b)),
        };
      }
    }
    if (typeof body.ramp_ms === 'number' && body.ramp_ms >= 0) {
      lightState.ramp_ms = body.ramp_ms;
    }
    lightState.last_changed_at = Date.now();
    lightHistory.push({ t: lightState.last_changed_at, v: lightState.on ? lightState.brightness_pct : 0 });
    while (lightHistory.length > 200) lightHistory.shift();
    res.json({ ...lightState, timestamp_us: Date.now() * 1000 });
  });
  app.get('/api/v1/actuator/light/history', (_req, res) => {
    // Return up to 60 samples covering the last hour. If the event log is
    // sparse (typical), pad with the current value at 1-min intervals so
    // the line mark always has a continuous trace.
    const now = Date.now();
    const samples: Array<{ t: number; v: number }> = [];
    for (let i = 59; i >= 0; i--) {
      const t = now - i * 60_000;
      // Find the latest event at or before t; fall back to current state.
      let v = lightState.on ? lightState.brightness_pct : 0;
      for (let j = lightHistory.length - 1; j >= 0; j--) {
        if (lightHistory[j]!.t <= t) { v = lightHistory[j]!.v; break; }
      }
      samples.push({ t, v });
    }
    res.json({ samples });
  });

  interface ThermostatState {
    setpoint_c: number;     // user-requested temp
    current_c: number;       // actual room temp (drifts toward setpoint)
    mode: 'heat' | 'cool' | 'off';
    last_changed_at: number;
  }
  const thermo: ThermostatState = {
    setpoint_c: 21.0, current_c: 19.5, mode: 'heat',
    last_changed_at: Date.now(),
  };
  // Drift current_c toward setpoint_c slowly — every snapshot read
  // re-computes based on elapsed time, so we don't need a timer.
  function thermoTick(now: number) {
    const dt = (now - thermo.last_changed_at) / 1000;  // seconds since last change
    if (thermo.mode === 'off') return;
    // ~0.1°C/min in either direction, capped at the setpoint.
    const rate = 0.1 / 60;     // °C/sec
    const diff = thermo.setpoint_c - thermo.current_c;
    if (Math.abs(diff) < 0.05) return;
    const step = Math.sign(diff) * Math.min(Math.abs(diff), rate * dt);
    thermo.current_c = Math.round((thermo.current_c + step) * 10) / 10;
    thermo.last_changed_at = now;
  }
  app.get('/api/v1/actuator/thermostat', (_req, res) => {
    const now = Date.now();
    thermoTick(now);
    res.json({ ...thermo, timestamp_us: now * 1000 });
  });
  app.post('/api/v1/actuator/thermostat', (req, res) => {
    const body = req.body as Partial<ThermostatState>;
    const now = Date.now();
    thermoTick(now);
    if (typeof body.setpoint_c === 'number') {
      thermo.setpoint_c = Math.max(10, Math.min(32, body.setpoint_c));
    }
    if (body.mode === 'heat' || body.mode === 'cool' || body.mode === 'off') {
      thermo.mode = body.mode;
    }
    thermo.last_changed_at = now;
    res.json({ ...thermo, timestamp_us: now * 1000 });
  });

  interface SpeakerState {
    last_played_id: string | null;
    last_played_at: number;       // ms epoch, 0 if never
    available_sounds: string[];
  }
  const speaker: SpeakerState = {
    last_played_id: null, last_played_at: 0,
    available_sounds: ['chime', 'doorbell', 'alarm', 'notification'],
  };
  app.get('/api/v1/actuator/speaker', (_req, res) => {
    const now = Date.now();
    res.json({ ...speaker, last_played_ago_ms: speaker.last_played_at ? now - speaker.last_played_at : null, timestamp_us: now * 1000 });
  });
  app.post('/api/v1/actuator/speaker/play', (req, res) => {
    const body = req.body as { sound_id?: string };
    const sound = body.sound_id ?? 'chime';
    if (!speaker.available_sounds.includes(sound)) {
      res.status(400).json({ error: 'unknown_sound', available: speaker.available_sounds });
      return;
    }
    speaker.last_played_id = sound;
    speaker.last_played_at = Date.now();
    res.json({ played: sound, at: speaker.last_played_at });
  });

  // ─── UC4 IMU simulation (P5a) ──────────────────────────────────────────
  //
  // Returns a deterministic noisy "airplane" attitude. Same time-derived
  // pattern as the body-temp / AQI / barometer feeds so the response is
  // reproducible for smoke baselines:
  //   - slow roll oscillation (±0.4 rad bank, 8-s period) — banking turns
  //   - shallower pitch oscillation (±0.15 rad, 12-s period) — climbing/descending
  //   - continuous slow yaw (one full turn per 30 s) — heading drift
  //   - acceleration carries gravity on the Y axis + small bumps on all axes
  //
  // This endpoint matches the shape of consumer IMU APIs (Euler radians +
  // m/s² for accel + rad/s for angular velocity) so the migration to a
  // real device is just a manifest URL swap — no client-side rewrite.
  function imuAt(t: number) {
    const m = t / 1000;                                  // seconds
    const roll  = 0.4  * Math.sin(m / 8);                 // ±0.4 rad
    const pitch = 0.15 * Math.sin(m / 12);                // ±0.15 rad
    const yaw   = (m / 30) % (2 * Math.PI);               // 0..2π
    return {
      orientation: {                                       // Euler, radians
        roll_rad:  Math.round(roll  * 1000) / 1000,
        pitch_rad: Math.round(pitch * 1000) / 1000,
        yaw_rad:   Math.round(yaw   * 1000) / 1000,
      },
      angular_velocity: {                                  // rad/s — derivative of the above
        x: Math.round((0.4  / 8)  * Math.cos(m / 8)  * 1000) / 1000,
        y: Math.round((1    / 30) * 1000) / 1000,
        z: Math.round((0.15 / 12) * Math.cos(m / 12) * 1000) / 1000,
      },
      acceleration: {                                      // m/s² — gravity on Y, small bumps elsewhere
        x: Math.round((0.05 * Math.sin(m * 1.3)) * 1000) / 1000,
        y: Math.round((9.81 + 0.3 * Math.sin(m / 4)) * 1000) / 1000,
        z: Math.round((0.05 * Math.sin(m * 0.7)) * 1000) / 1000,
      },
    };
  }
  app.get('/api/v1/sensor/imu', (_req, res) => {
    const now = Date.now();
    res.json({ ...imuAt(now), timestamp_us: now * 1000 });
  });

  return {
    app,
    getCurrentCode: () => currentCode,
    getPreviousCode: () => previousCode,
    rotateCode,
    resetCounter: () => { codeCounter = 0; currentCode = generateCode(); previousCode = null; expiredCodes.clear(); },
    stopRotationTimer: () => { if (rotationTimer) { clearInterval(rotationTimer); rotationTimer = null; } },
  };
}

// ─── CLI entry point ────────────────────────────────────────────────

const isMain = process.argv[1] === __filename;
if (isMain) {
  const PORT = 3001;
  const ROTATION_SECONDS = parseInt(process.env.CODE_ROTATION_SECONDS ?? '300', 10);
  const server = createJoinServer({ rotationSeconds: ROTATION_SECONDS });

  console.log(`[join-server] Initial code: ${server.getCurrentCode()} (rotates every ${ROTATION_SECONDS}s)`);
  console.log(`[join-server] Set CODE_ROTATION_SECONDS env var to change (current: ${ROTATION_SECONDS}s)`);
  console.log(`[join-server] Fixed UC codes:`);
  for (const [code, path] of Object.entries(DEFAULT_FIXED_CODES)) {
    console.log(`  ${code}  →  ${path}`);
  }

  server.app.listen(PORT, () => {
    console.log(`[join-server] Mock join server running on http://localhost:${PORT}`);
    console.log(`[join-server] Endpoints:`);
    console.log(`  POST /api/v1/join   — validate a join code`);
    console.log(`  GET  /api/v1/manifest — fetch manifest (requires JWT)`);
    console.log(`  GET  /api/v1/code   — get current code (dev only)`);
  });
}
