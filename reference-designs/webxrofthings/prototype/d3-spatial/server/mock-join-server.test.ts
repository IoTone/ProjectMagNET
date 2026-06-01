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

  it('uses only characters from the full alphanumeric set', () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false });
    for (let i = 0; i < 100; i++) s.rotateCode();
    const code = s.getCurrentCode();
    for (const ch of code) {
      expect(CHAR_SET).toContain(ch);
    }
    s.stopRotationTimer();
  });

  it('generates codes sequentially and tracks previousCode after rotation', () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false });
    expect(CHAR_SET.length).toBe(36); // A-Z (26) + 0-9 (10)
    expect(s.getCurrentCode()).toBe('AAAAAA');
    s.rotateCode();
    // After one rotation, the old AAAAAA becomes previous; the new code is the next sequential
    expect(s.getPreviousCode()).toBe('AAAAAA');
    expect(s.getCurrentCode()).not.toBe('AAAAAA');
    expect(s.getCurrentCode()).toMatch(/^[A-Z0-9]{6}$/);
    s.stopRotationTimer();
  });

  it('CHAR_SET includes the characters required to type the fixed DEMO codes', () => {
    // Regression guard for the P1+P5 era. DEMOXX codes contain O / 0 / 1
    // which the prior ambiguity-stripped set excluded — they were untypable
    // in the slot wheel until this change.
    for (const ch of 'DEMO0123456789') {
      expect(CHAR_SET).toContain(ch);
    }
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

describe('mock-join-server — fixed UC codes', () => {
  // Build server with custom fixed codes pointing at fixtures we know exist.
  // Using examples/* keeps the test grounded in real manifests instead of mocks.
  function buildWithFixedCodes() {
    return createJoinServer({
      jwtSecret: JWT_SECRET,
      startRotationTimer: false,
      rateLimitPerMinute: 1000,
    });
  }

  it.each([
    ['DEMO01', 'demo01'],
    ['DEMO02', 'demo02'],
    ['DEMO03', 'demo03'],
    ['DEMO04', 'demo04'],
  ])('%s is accepted and resolves to dataspace=%s', async (code, dataspace) => {
    const s = buildWithFixedCodes();
    const res = await request(s.app).post('/api/v1/join').send({ code });
    expect(res.status).toBe(200);
    expect(res.body.status).toBe('accepted');
    expect(res.body.dataspace).toBe(dataspace);
    expect(typeof res.body.token).toBe('string');
    s.stopRotationTimer();
  });

  it('is case-insensitive (demo03 → DEMO03)', async () => {
    const s = buildWithFixedCodes();
    const res = await request(s.app).post('/api/v1/join').send({ code: 'demo03' });
    expect(res.body.status).toBe('accepted');
    expect(res.body.dataspace).toBe('demo03');
    s.stopRotationTimer();
  });

  it('serves the per-code manifest, not the rotating-code default', async () => {
    const s = buildWithFixedCodes();
    // UC1 manifest has the vitals device; UC3 has the gallery marks.
    // Validating each code should fetch a manifest with the corresponding shape.
    const j1 = await request(s.app).post('/api/v1/join').send({ code: 'DEMO01' });
    const m1 = await request(s.app)
      .get('/api/v1/manifest')
      .set('Authorization', `Bearer ${j1.body.token}`);
    expect(m1.status).toBe(200);
    expect(m1.body.name).toBe('kords-personal-health');
    expect(Array.isArray(m1.body.udm_devices)).toBe(true);
    expect(m1.body.udm_devices.length).toBeGreaterThan(0);

    const j3 = await request(s.app).post('/api/v1/join').send({ code: 'DEMO03' });
    const m3 = await request(s.app)
      .get('/api/v1/manifest')
      .set('Authorization', `Bearer ${j3.body.token}`);
    expect(m3.status).toBe(200);
    expect(m3.body.name).toBe('uc3-xrt-exhibit');
    expect(Array.isArray(m3.body.marks)).toBe(true);

    s.stopRotationTimer();
  });

  it('falls back to the default manifest for tokens minted by the rotating code', async () => {
    const s = buildWithFixedCodes();
    const j = await request(s.app).post('/api/v1/join').send({ code: s.getCurrentCode() });
    const m = await request(s.app)
      .get('/api/v1/manifest')
      .set('Authorization', `Bearer ${j.body.token}`);
    expect(m.status).toBe(200);
    // Default is examples/room-dataspace.json — name is 'lab-room-alpha'.
    expect(m.body.name).toBe('lab-room-alpha');
    s.stopRotationTimer();
  });

  it('rejects an unknown code that resembles a fixed code (e.g. DEMO99)', async () => {
    const s = buildWithFixedCodes();
    const res = await request(s.app).post('/api/v1/join').send({ code: 'DEMO99' });
    expect(res.body.status).toBe('rejected');
    s.stopRotationTimer();
  });

  it('honors a custom fixedCodes map', async () => {
    const s = createJoinServer({
      jwtSecret: JWT_SECRET,
      startRotationTimer: false,
      rateLimitPerMinute: 1000,
      // Use uc4-airplane.json as the target so we can distinguish it from defaults.
      fixedCodes: { TEST01: 'uc4-airplane.json' },
    });
    const j = await request(s.app).post('/api/v1/join').send({ code: 'TEST01' });
    expect(j.body.status).toBe('accepted');
    expect(j.body.dataspace).toBe('test01');
    const m = await request(s.app)
      .get('/api/v1/manifest')
      .set('Authorization', `Bearer ${j.body.token}`);
    expect(m.body.name).toBe('uc4-airplane');
    s.stopRotationTimer();
  });

  it('default-untouched DEMO01 fixed code is NOT also accepted as the rotating code', async () => {
    // Belt-and-suspenders: confirm fixed codes don't accidentally enter the
    // expiredCodes set when the rotating code rotates past them.
    const s = buildWithFixedCodes();
    for (let i = 0; i < 50; i++) s.rotateCode();
    const res = await request(s.app).post('/api/v1/join').send({ code: 'DEMO01' });
    expect(res.body.status).toBe('accepted');
    expect(res.body.dataspace).toBe('demo01');
    s.stopRotationTimer();
  });
});

describe('mock-join-server — simulated body-temperature feed (P3)', () => {
  it('GET /api/v1/sensor/body-temperature/history returns 60 in-band samples', async () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
    const res = await request(s.app).get('/api/v1/sensor/body-temperature/history');
    expect(res.status).toBe(200);
    expect(Array.isArray(res.body.samples)).toBe(true);
    expect(res.body.samples.length).toBe(60);
    for (const sample of res.body.samples) {
      expect(typeof sample.t).toBe('number');
      expect(typeof sample.v).toBe('number');
      // Generator wanders ~36.4 - 37.0 °C; assert wider plausible-body band.
      expect(sample.v).toBeGreaterThan(36.0);
      expect(sample.v).toBeLessThan(37.5);
    }
    s.stopRotationTimer();
  });

  it('history samples are monotonically time-ascending and 1 minute apart', async () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
    const res = await request(s.app).get('/api/v1/sensor/body-temperature/history');
    const samples: Array<{ t: number; v: number }> = res.body.samples;
    for (let i = 1; i < samples.length; i++) {
      const dt = samples[i]!.t - samples[i - 1]!.t;
      expect(dt).toBe(60_000);
    }
    s.stopRotationTimer();
  });

  it('GET /api/v1/sensor/body-temperature returns a snapshot with celsius + timestamp', async () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
    const res = await request(s.app).get('/api/v1/sensor/body-temperature');
    expect(res.status).toBe(200);
    expect(typeof res.body.celsius).toBe('number');
    expect(res.body.celsius).toBeGreaterThan(36.0);
    expect(res.body.celsius).toBeLessThan(37.5);
    expect(typeof res.body.timestamp_us).toBe('number');
    s.stopRotationTimer();
  });

  it('snapshot value matches the most-recent history sample (within numerical jitter)', async () => {
    const s = createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
    const snap = await request(s.app).get('/api/v1/sensor/body-temperature');
    const hist = await request(s.app).get('/api/v1/sensor/body-temperature/history');
    const last = hist.body.samples[hist.body.samples.length - 1].v;
    // Same generator, calls fractions of a ms apart — values should round-equal.
    expect(Math.abs(snap.body.celsius - last)).toBeLessThan(0.01);
    s.stopRotationTimer();
  });
});

describe('mock-join-server — UC2 environmental sensors (P4a)', () => {
  function buildEnv() {
    return createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
  }

  /* ─── AQI ─── */
  it('AQI history returns 60 in-band integer samples', async () => {
    const s = buildEnv();
    const res = await request(s.app).get('/api/v1/sensor/aqi/history');
    expect(res.status).toBe(200);
    expect(res.body.samples.length).toBe(60);
    for (const sample of res.body.samples) {
      expect(typeof sample.t).toBe('number');
      expect(Number.isInteger(sample.v)).toBe(true);   // AQI is rounded to int
      expect(sample.v).toBeGreaterThanOrEqual(0);
      expect(sample.v).toBeLessThan(150);              // generator stays sub-100, allow margin
    }
    s.stopRotationTimer();
  });

  it('AQI snapshot returns aqi + a valid category', async () => {
    const s = buildEnv();
    const res = await request(s.app).get('/api/v1/sensor/aqi');
    expect(res.status).toBe(200);
    expect(typeof res.body.aqi).toBe('number');
    expect(['good', 'moderate', 'unhealthy-for-sensitive', 'unhealthy', 'very-unhealthy', 'hazardous'])
      .toContain(res.body.category);
    s.stopRotationTimer();
  });

  /* ─── Barometer ─── */
  it('barometer history returns 60 in-band hPa samples', async () => {
    const s = buildEnv();
    const res = await request(s.app).get('/api/v1/sensor/barometer/history');
    expect(res.body.samples.length).toBe(60);
    for (const sample of res.body.samples) {
      expect(sample.v).toBeGreaterThan(1005);
      expect(sample.v).toBeLessThan(1025);
    }
    s.stopRotationTimer();
  });

  it('barometer snapshot includes hpa + a valid trend', async () => {
    const s = buildEnv();
    const res = await request(s.app).get('/api/v1/sensor/barometer');
    expect(typeof res.body.hpa).toBe('number');
    expect(['rising', 'falling', 'steady']).toContain(res.body.trend);
    s.stopRotationTimer();
  });

  /* ─── Pollen ─── */
  it('pollen history returns 60 non-negative samples bounded by ~12', async () => {
    const s = buildEnv();
    const res = await request(s.app).get('/api/v1/sensor/pollen/history');
    expect(res.body.samples.length).toBe(60);
    for (const sample of res.body.samples) {
      expect(sample.v).toBeGreaterThanOrEqual(0);
      expect(sample.v).toBeLessThan(12);
    }
    s.stopRotationTimer();
  });

  it('pollen snapshot returns count + a valid level', async () => {
    const s = buildEnv();
    const res = await request(s.app).get('/api/v1/sensor/pollen');
    expect(typeof res.body.count).toBe('number');
    expect(['low', 'moderate', 'high', 'very-high']).toContain(res.body.level);
    s.stopRotationTimer();
  });

  /* ─── snapshot/history coherence ─── */
  it.each([
    ['aqi'],
    ['barometer'],
    ['pollen'],
  ])('%s snapshot value matches the most-recent history sample', async (key) => {
    const s = buildEnv();
    const snap = await request(s.app).get(`/api/v1/sensor/${key}`);
    const hist = await request(s.app).get(`/api/v1/sensor/${key}/history`);
    const last = hist.body.samples[hist.body.samples.length - 1].v;
    const snapValue = snap.body.aqi ?? snap.body.hpa ?? snap.body.count;
    // AQI snapshot is integer, history is integer; baro is 0.1-rounded; pollen 0.1-rounded.
    // Allow ≤1 unit jitter for AQI (integer rounding) and 0.1 for the others.
    const tolerance = key === 'aqi' ? 1 : 0.11;
    expect(Math.abs(snapValue - last)).toBeLessThanOrEqual(tolerance);
    s.stopRotationTimer();
  });
});

describe('mock-join-server — UC2 actuators (P4b)', () => {
  function buildAct() {
    return createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
  }

  /* ─── Lighting ─── */
  it('GET /api/v1/actuator/light returns the default state (on, ~70%, warm white)', async () => {
    const s = buildAct();
    const res = await request(s.app).get('/api/v1/actuator/light');
    expect(res.status).toBe(200);
    expect(res.body.on).toBe(true);
    expect(res.body.brightness_pct).toBe(70);
    expect(res.body.color).toEqual({ r: 255, g: 220, b: 180 });
    expect(res.body.ramp_ms).toBe(500);
    s.stopRotationTimer();
  });

  it('POST mutates lighting brightness and persists across reads', async () => {
    const s = buildAct();
    await request(s.app).post('/api/v1/actuator/light').send({ brightness_pct: 35 });
    const res = await request(s.app).get('/api/v1/actuator/light');
    expect(res.body.brightness_pct).toBe(35);
    expect(res.body.on).toBe(true);
    s.stopRotationTimer();
  });

  it('POST clamps brightness to [0, 100]', async () => {
    const s = buildAct();
    await request(s.app).post('/api/v1/actuator/light').send({ brightness_pct: 250 });
    const high = await request(s.app).get('/api/v1/actuator/light');
    expect(high.body.brightness_pct).toBe(100);
    await request(s.app).post('/api/v1/actuator/light').send({ brightness_pct: -10 });
    const low = await request(s.app).get('/api/v1/actuator/light');
    expect(low.body.brightness_pct).toBe(0);
    s.stopRotationTimer();
  });

  it('POST color updates RGB, clamping each channel to [0, 255]', async () => {
    const s = buildAct();
    await request(s.app).post('/api/v1/actuator/light').send({ color: { r: 999, g: 100, b: -50 } });
    const res = await request(s.app).get('/api/v1/actuator/light');
    expect(res.body.color).toEqual({ r: 255, g: 100, b: 0 });
    s.stopRotationTimer();
  });

  it('light/history records 0 brightness whenever the light is off', async () => {
    const s = buildAct();
    await request(s.app).post('/api/v1/actuator/light').send({ on: false, brightness_pct: 80 });
    const hist = await request(s.app).get('/api/v1/actuator/light/history');
    // The most-recent sample reflects current state — off means 0 in the line.
    expect(hist.body.samples[hist.body.samples.length - 1].v).toBe(0);
    s.stopRotationTimer();
  });

  /* ─── Thermostat ─── */
  it('GET /api/v1/actuator/thermostat returns defaults (21°C setpoint, 19.5°C current, heat)', async () => {
    const s = buildAct();
    const res = await request(s.app).get('/api/v1/actuator/thermostat');
    expect(res.body.setpoint_c).toBe(21.0);
    expect(res.body.mode).toBe('heat');
    // current_c may have drifted a hair on the first read but stays close to 19.5.
    expect(Math.abs(res.body.current_c - 19.5)).toBeLessThan(0.5);
    s.stopRotationTimer();
  });

  it('POST clamps setpoint to [10, 32] °C', async () => {
    const s = buildAct();
    await request(s.app).post('/api/v1/actuator/thermostat').send({ setpoint_c: 50 });
    const high = await request(s.app).get('/api/v1/actuator/thermostat');
    expect(high.body.setpoint_c).toBe(32);
    await request(s.app).post('/api/v1/actuator/thermostat').send({ setpoint_c: 0 });
    const low = await request(s.app).get('/api/v1/actuator/thermostat');
    expect(low.body.setpoint_c).toBe(10);
    s.stopRotationTimer();
  });

  it('POST mode accepts heat/cool/off, ignores anything else', async () => {
    const s = buildAct();
    await request(s.app).post('/api/v1/actuator/thermostat').send({ mode: 'cool' });
    expect((await request(s.app).get('/api/v1/actuator/thermostat')).body.mode).toBe('cool');
    await request(s.app).post('/api/v1/actuator/thermostat').send({ mode: 'auto' });   // unknown
    expect((await request(s.app).get('/api/v1/actuator/thermostat')).body.mode).toBe('cool');   // unchanged
    s.stopRotationTimer();
  });

  /* ─── Speaker ─── */
  it('GET /api/v1/actuator/speaker returns the available sound list and a never-played state on boot', async () => {
    const s = buildAct();
    const res = await request(s.app).get('/api/v1/actuator/speaker');
    expect(res.body.last_played_id).toBeNull();
    expect(res.body.last_played_ago_ms).toBeNull();
    expect(res.body.available_sounds).toContain('chime');
    expect(res.body.available_sounds).toContain('doorbell');
    s.stopRotationTimer();
  });

  it('POST /play records the played sound and elapsed time', async () => {
    const s = buildAct();
    const play = await request(s.app).post('/api/v1/actuator/speaker/play').send({ sound_id: 'chime' });
    expect(play.body.played).toBe('chime');
    expect(typeof play.body.at).toBe('number');
    const after = await request(s.app).get('/api/v1/actuator/speaker');
    expect(after.body.last_played_id).toBe('chime');
    expect(after.body.last_played_ago_ms).toBeGreaterThanOrEqual(0);
    expect(after.body.last_played_ago_ms).toBeLessThan(1000);
    s.stopRotationTimer();
  });

  it('POST /play rejects unknown sound_id with 400 and lists available sounds', async () => {
    const s = buildAct();
    const res = await request(s.app).post('/api/v1/actuator/speaker/play').send({ sound_id: 'fart' });
    expect(res.status).toBe(400);
    expect(res.body.error).toBe('unknown_sound');
    expect(Array.isArray(res.body.available)).toBe(true);
    s.stopRotationTimer();
  });

  it('POST /play with no sound_id defaults to "chime"', async () => {
    const s = buildAct();
    const play = await request(s.app).post('/api/v1/actuator/speaker/play').send({});
    expect(play.body.played).toBe('chime');
    s.stopRotationTimer();
  });
});

describe('mock-join-server — UC2 NeoPixel strip', () => {
  function buildNeo() {
    return createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
  }

  it('GET returns the default state + available_patterns enum', async () => {
    const s = buildNeo();
    const res = await request(s.app).get('/api/v1/actuator/neopixel');
    expect(res.status).toBe(200);
    expect(res.body.on).toBe(true);
    expect(res.body.brightness_pct).toBe(80);
    expect(res.body.color).toEqual({ r: 0, g: 200, b: 255 });
    expect(res.body.pattern).toBe('rainbow');
    expect(res.body.pattern_speed_pct).toBe(50);
    expect(res.body.led_count).toBe(60);
    expect(res.body.available_patterns).toEqual(
      ['solid', 'breathing', 'rainbow', 'chase', 'twinkle'],
    );
    s.stopRotationTimer();
  });

  it('POST mutates pattern + speed and persists across reads', async () => {
    const s = buildNeo();
    await request(s.app).post('/api/v1/actuator/neopixel')
      .send({ pattern: 'chase', pattern_speed_pct: 80 });
    const res = await request(s.app).get('/api/v1/actuator/neopixel');
    expect(res.body.pattern).toBe('chase');
    expect(res.body.pattern_speed_pct).toBe(80);
    s.stopRotationTimer();
  });

  it('POST clamps brightness + pattern_speed to [0, 100]', async () => {
    const s = buildNeo();
    await request(s.app).post('/api/v1/actuator/neopixel')
      .send({ brightness_pct: 250, pattern_speed_pct: -10 });
    const res = await request(s.app).get('/api/v1/actuator/neopixel');
    expect(res.body.brightness_pct).toBe(100);
    expect(res.body.pattern_speed_pct).toBe(0);
    s.stopRotationTimer();
  });

  it('POST color clamps each RGB channel to [0, 255]', async () => {
    const s = buildNeo();
    await request(s.app).post('/api/v1/actuator/neopixel')
      .send({ color: { r: 300, g: 128, b: -5 } });
    const res = await request(s.app).get('/api/v1/actuator/neopixel');
    expect(res.body.color).toEqual({ r: 255, g: 128, b: 0 });
    s.stopRotationTimer();
  });

  it('POST ignores unknown pattern values, leaves prior value in place', async () => {
    const s = buildNeo();
    await request(s.app).post('/api/v1/actuator/neopixel').send({ pattern: 'rainbow' });
    await request(s.app).post('/api/v1/actuator/neopixel').send({ pattern: 'disco-inferno' });
    expect((await request(s.app).get('/api/v1/actuator/neopixel')).body.pattern).toBe('rainbow');
    s.stopRotationTimer();
  });

  it('POST on=false turns the strip off without losing color/pattern state', async () => {
    const s = buildNeo();
    await request(s.app).post('/api/v1/actuator/neopixel')
      .send({ on: false, color: { r: 50, g: 60, b: 70 }, pattern: 'breathing' });
    const res = await request(s.app).get('/api/v1/actuator/neopixel');
    expect(res.body.on).toBe(false);
    // The color and pattern should still be retained — turning it back on
    // shouldn't reset the look.
    expect(res.body.color).toEqual({ r: 50, g: 60, b: 70 });
    expect(res.body.pattern).toBe('breathing');
    s.stopRotationTimer();
  });
});

describe('mock-join-server — UC4 IMU sim (P5a)', () => {
  function buildImu() {
    return createJoinServer({ jwtSecret: JWT_SECRET, startRotationTimer: false, rateLimitPerMinute: 1000 });
  }

  it('GET /api/v1/sensor/imu returns the expected shape', async () => {
    const s = buildImu();
    const res = await request(s.app).get('/api/v1/sensor/imu');
    expect(res.status).toBe(200);
    // Orientation in radians, three Euler components.
    expect(typeof res.body.orientation.roll_rad).toBe('number');
    expect(typeof res.body.orientation.pitch_rad).toBe('number');
    expect(typeof res.body.orientation.yaw_rad).toBe('number');
    // Angular velocity in rad/s.
    expect(typeof res.body.angular_velocity.x).toBe('number');
    expect(typeof res.body.angular_velocity.y).toBe('number');
    expect(typeof res.body.angular_velocity.z).toBe('number');
    // Linear acceleration in m/s².
    expect(typeof res.body.acceleration.x).toBe('number');
    expect(typeof res.body.acceleration.y).toBe('number');
    expect(typeof res.body.acceleration.z).toBe('number');
    expect(typeof res.body.timestamp_us).toBe('number');
    s.stopRotationTimer();
  });

  it('orientation is within plausible airplane attitude bounds', async () => {
    const s = buildImu();
    const res = await request(s.app).get('/api/v1/sensor/imu');
    // Roll is ±0.4 rad max (banking turn), pitch ±0.15 rad max (gentle
    // climb/descent), yaw 0..2π (any heading). Allow margins for rounding.
    expect(Math.abs(res.body.orientation.roll_rad)).toBeLessThanOrEqual(0.5);
    expect(Math.abs(res.body.orientation.pitch_rad)).toBeLessThanOrEqual(0.25);
    expect(res.body.orientation.yaw_rad).toBeGreaterThanOrEqual(0);
    expect(res.body.orientation.yaw_rad).toBeLessThan(2 * Math.PI);
    s.stopRotationTimer();
  });

  it('acceleration shows gravity on the Y axis', async () => {
    // Gravity dominates Y (cruise — small ±0.3 m/s² bumps superimposed).
    // X and Z are small noise (±0.05 m/s² envelope).
    const s = buildImu();
    const res = await request(s.app).get('/api/v1/sensor/imu');
    expect(res.body.acceleration.y).toBeGreaterThan(9.4);
    expect(res.body.acceleration.y).toBeLessThan(10.2);
    expect(Math.abs(res.body.acceleration.x)).toBeLessThan(0.1);
    expect(Math.abs(res.body.acceleration.z)).toBeLessThan(0.1);
    s.stopRotationTimer();
  });

  it('is deterministic within the same wall-clock millisecond', async () => {
    // Two back-to-back requests in <1ms should round to identical samples.
    const s = buildImu();
    const a = await request(s.app).get('/api/v1/sensor/imu');
    const b = await request(s.app).get('/api/v1/sensor/imu');
    // Yaw is cumulative-time-driven so it strictly advances; the orientation
    // values are 3-decimal-rounded so within sub-ms calls they coincide
    // unless the wall-clock crossed a quantisation boundary. Use a tight
    // numerical tolerance instead of strict equality.
    expect(Math.abs(a.body.orientation.roll_rad  - b.body.orientation.roll_rad )).toBeLessThan(0.01);
    expect(Math.abs(a.body.orientation.pitch_rad - b.body.orientation.pitch_rad)).toBeLessThan(0.01);
    s.stopRotationTimer();
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
