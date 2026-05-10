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

// Ambiguity-stripped character set: A-Z minus O/I/L, digits 2-9 (no 0/1)
export const CHAR_SET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
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
