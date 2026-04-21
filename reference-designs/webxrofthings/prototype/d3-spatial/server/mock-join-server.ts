/**
 * Mock Join Server — P1.1
 *
 * Generates rotating 6-char join codes and validates them via POST /api/v1/join.
 * Serves the room-dataspace manifest via GET /api/v1/manifest (JWT-protected).
 * Exposes GET /api/v1/code for development convenience.
 */

import express from 'express';
import jwt from 'jsonwebtoken';
import { readFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import crypto from 'crypto';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const app = express();
app.use(express.json());

const PORT = 3001;
const JWT_SECRET = 'hlxr-dev-secret';
const ROTATION_SECONDS = parseInt(process.env.CODE_ROTATION_SECONDS ?? '300', 10); // default 5 minutes
const CODE_LENGTH = 6;

// Ambiguity-stripped character set: A-Z minus O/I/L, digits 2-9 (no 0/1)
const CHAR_SET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';

// ─── Code rotation (sequential from AAAAAA) ─────────────────────────

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
let expiredCodes: Set<string> = new Set();
let codeGeneratedAt = Date.now();

function rotateCode() {
  if (previousCode) {
    expiredCodes.add(previousCode);
    // Only keep last 10 expired codes to avoid unbounded growth
    if (expiredCodes.size > 10) {
      const first = expiredCodes.values().next().value;
      if (first !== undefined) expiredCodes.delete(first);
    }
  }
  previousCode = currentCode;
  currentCode = generateCode();
  codeGeneratedAt = Date.now();
  console.log(`[join-server] New code: ${currentCode} (rotates in ${ROTATION_SECONDS}s)`);
}

// Rotate every ROTATION_SECONDS
setInterval(rotateCode, ROTATION_SECONDS * 1000);
console.log(`[join-server] Initial code: ${currentCode} (rotates every ${ROTATION_SECONDS}s)`);
console.log(`[join-server] Set CODE_ROTATION_SECONDS env var to change (current: ${ROTATION_SECONDS}s)`);

// ─── Rate limiting ──────────────────────────────────────────────────

const rateLimits = new Map<string, { count: number; windowStart: number }>();

function isRateLimited(ip: string): boolean {
  const now = Date.now();
  const entry = rateLimits.get(ip);
  if (!entry || now - entry.windowStart > 60_000) {
    rateLimits.set(ip, { count: 1, windowStart: now });
    return false;
  }
  entry.count++;
  return entry.count > 5;
}

// ─── Routes ─────────────────────────────────────────────────────────

// POST /api/v1/join — validate a join code
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

  // Check current or previous code (grace period)
  if (upper === currentCode || upper === previousCode) {
    const session = crypto.randomUUID();
    const token = jwt.sign(
      { dataspace: 'demo-room', session, iat: Math.floor(Date.now() / 1000) },
      JWT_SECRET,
      { expiresIn: '1h' },
    );
    console.log(`[join-server] Code ${upper} accepted (session: ${session.slice(0, 8)}...)`);
    res.json({
      status: 'accepted',
      token,
      manifest_url: '/api/v1/manifest',
      dataspace: 'demo-room',
    });
    return;
  }

  // Check expired codes (2+ rotations ago)
  if (expiredCodes.has(upper)) {
    res.json({ status: 'rejected', reason: 'expired' });
    return;
  }

  res.json({ status: 'rejected', reason: 'invalid' });
});

// GET /api/v1/manifest — return room-dataspace manifest (JWT required)
app.get('/api/v1/manifest', (req, res) => {
  const auth = req.headers.authorization;
  if (!auth || !auth.startsWith('Bearer ')) {
    res.status(401).json({ error: 'missing_token' });
    return;
  }

  const token = auth.slice(7);
  try {
    jwt.verify(token, JWT_SECRET);
  } catch {
    res.status(401).json({ error: 'invalid_token' });
    return;
  }

  const manifestPath = join(__dirname, '..', 'examples', 'room-dataspace.json');
  try {
    const manifest = JSON.parse(readFileSync(manifestPath, 'utf-8'));
    res.json(manifest);
  } catch (e) {
    console.error('[join-server] Failed to read manifest:', e);
    res.status(500).json({ error: 'manifest_read_failed' });
  }
});

// GET /api/v1/code — development convenience endpoint
app.get('/api/v1/code', (_req, res) => {
  const elapsed = (Date.now() - codeGeneratedAt) / 1000;
  const expiresIn = Math.max(0, Math.round(ROTATION_SECONDS - elapsed));
  res.json({ code: currentCode, expiresIn });
});

// ─── Start ──────────────────────────────────────────────────────────

app.listen(PORT, () => {
  console.log(`[join-server] Mock join server running on http://localhost:${PORT}`);
  console.log(`[join-server] Endpoints:`);
  console.log(`  POST /api/v1/join   — validate a join code`);
  console.log(`  GET  /api/v1/manifest — fetch manifest (requires JWT)`);
  console.log(`  GET  /api/v1/code   — get current code (dev only)`);
});
