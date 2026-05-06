import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import request from 'supertest';
import jwt from 'jsonwebtoken';
import { createJoinServer, CHAR_SET, type JoinServer } from './mock-join-server';

const JWT_SECRET = 'test-secret';

describe('mock-join-server — code generator', () => {
  it('starts at AAAAAA', () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false });
    expect(s.getCurrentCode()).toBe('AAAAAA');
    s.stopRotationTimer();
  });

  it('uses only ambiguity-stripped characters', () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false });
    for (let i = 0; i < 100; i++) s.rotateCode();
    const code = s.getCurrentCode();
    for (const ch of code) {
      expect(CHAR_SET).toContain(ch);
      expect('OIL01'.includes(ch)).toBe(false);
    }
    s.stopRotationTimer();
  });

  it('generates codes sequentially and tracks previousCode after rotation', () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false });
    expect(CHAR_SET.length).toBe(31); // A-Z minus O/I/L = 23 letters + 2-9 = 8 digits
    expect(s.getCurrentCode()).toBe('AAAAAA');
    s.rotateCode();
    // After one rotation, the old AAAAAA becomes previous; the new code is the next sequential
    expect(s.getPreviousCode()).toBe('AAAAAA');
    expect(s.getCurrentCode()).not.toBe('AAAAAA');
    expect(s.getCurrentCode()).toMatch(/^[A-Z2-9]{6}$/);
    s.stopRotationTimer();
  });
});

describe('mock-join-server — POST /api/v1/join', () => {
  let s: JoinServer;

  beforeEach(() => {
    s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
  });

  afterEach(() => {
    s.stopRotationTimer();
  });

  it('accepts the current code and issues a JWT', async () => {
    const res = await request(s.app)
      .post('/api/v1/join')
      .send({ code: s.getCurrentCode() });
    expect(res.status).toBe(200);
    expect(res.body.status).toBe('accepted');
    expect(res.body.token).toBeTypeOf('string');

    const decoded = jwt.verify(res.body.token, JWT_SECRET) as { dataspace: string };
    expect(decoded.dataspace).toBe('demo-room');
  });

  it('accepts the previous code (rotation grace window)', async () => {
    const oldCode = s.getCurrentCode();
    s.rotateCode();
    const res = await request(s.app)
      .post('/api/v1/join')
      .send({ code: oldCode });
    expect(res.status).toBe(200);
    expect(res.body.status).toBe('accepted');
  });

  it('is case-insensitive', async () => {
    const res = await request(s.app)
      .post('/api/v1/join')
      .send({ code: s.getCurrentCode().toLowerCase() });
    expect(res.body.status).toBe('accepted');
  });

  it('rejects an unknown code as invalid', async () => {
    const res = await request(s.app)
      .post('/api/v1/join')
      .send({ code: 'ZZZZZZ' });
    expect(res.status).toBe(200);
    expect(res.body.status).toBe('rejected');
    expect(res.body.reason).toBe('invalid');
  });

  it('rejects a code that has rotated out of the grace window as expired', async () => {
    const oldCode = s.getCurrentCode();
    s.rotateCode(); // oldCode → previousCode
    s.rotateCode(); // previousCode → expired set
    const res = await request(s.app)
      .post('/api/v1/join')
      .send({ code: oldCode });
    expect(res.body.status).toBe('rejected');
    expect(res.body.reason).toBe('expired');
  });

  it('rejects missing or non-string code with 400', async () => {
    const res = await request(s.app).post('/api/v1/join').send({});
    expect(res.status).toBe(400);
  });

  it('rate-limits after 5 attempts per minute', async () => {
    const tight = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 5 });
    let lastStatus = 0;
    for (let i = 0; i < 7; i++) {
      const res = await request(tight.app).post('/api/v1/join').send({ code: 'XXXXXX' });
      lastStatus = res.status;
    }
    expect(lastStatus).toBe(429);
    tight.stopRotationTimer();
  });
});

describe('mock-join-server — GET /api/v1/manifest', () => {
  let s: JoinServer;

  beforeEach(() => {
    s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false });
  });

  afterEach(() => {
    s.stopRotationTimer();
  });

  it('rejects requests without an Authorization header', async () => {
    const res = await request(s.app).get('/api/v1/manifest');
    expect(res.status).toBe(401);
    expect(res.body.error).toBe('missing_token');
  });

  it('rejects requests with an invalid JWT', async () => {
    const res = await request(s.app)
      .get('/api/v1/manifest')
      .set('Authorization', 'Bearer not-a-real-token');
    expect(res.status).toBe(401);
    expect(res.body.error).toBe('invalid_token');
  });

  it('rejects a JWT signed with a different secret', async () => {
    const otherToken = jwt.sign({ dataspace: 'demo-room' }, 'wrong-secret', { expiresIn: '1h' });
    const res = await request(s.app)
      .get('/api/v1/manifest')
      .set('Authorization', `Bearer ${otherToken}`);
    expect(res.status).toBe(401);
  });

  it('returns the manifest for a valid JWT', async () => {
    const join = await request(s.app).post('/api/v1/join').send({ code: s.getCurrentCode() });
    const token = join.body.token;
    const res = await request(s.app)
      .get('/api/v1/manifest')
      .set('Authorization', `Bearer ${token}`);
    expect(res.status).toBe(200);
    expect(res.body.version).toBe('1');
    expect(Array.isArray(res.body.marks)).toBe(true);
  });
});

describe('mock-join-server — GET /api/v1/code', () => {
  it('returns current code and a positive expiresIn', async () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, rotationSeconds: 300, startRotationTimer: false });
    const res = await request(s.app).get('/api/v1/code');
    expect(res.status).toBe(200);
    expect(res.body.code).toBe(s.getCurrentCode());
    expect(res.body.expiresIn).toBeGreaterThan(0);
    expect(res.body.expiresIn).toBeLessThanOrEqual(300);
    s.stopRotationTimer();
  });
});
