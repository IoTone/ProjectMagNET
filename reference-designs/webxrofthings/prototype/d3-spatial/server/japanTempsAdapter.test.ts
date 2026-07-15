import { describe, it, expect } from 'vitest';
import {
  jmaTimestampToPath, prefixesFor, extractPrefectureTemps,
  createJapanTempsProvider, RECHECK_MIN_MS,
} from './japanTempsAdapter';
import { JAPAN_PREFECTURES } from '../src/demo/japanPrefectures';

/* ─── pure helpers ───────────────────────────────────────────────────── */

describe('jmaTimestampToPath', () => {
  it('converts the JST ISO string to the map filename stem', () => {
    expect(jmaTimestampToPath('2026-07-14T01:00:00+09:00')).toBe('20260714010000');
  });
  it('rejects garbage', () => {
    expect(jmaTimestampToPath('not a time')).toBeNull();
    expect(jmaTimestampToPath('')).toBeNull();
  });
});

describe('prefixesFor', () => {
  it('uses the station area code for ordinary prefectures', () => {
    expect(prefixesFor(13, '44132')).toEqual(['44']);
  });
  it('spans 11–24 for Hokkaido and 91–94 for Okinawa', () => {
    expect(prefixesFor(1, '14163')).toContain('11');
    expect(prefixesFor(1, '14163')).toContain('24');
    expect(prefixesFor(47, '91197')).toEqual(['91', '92', '93', '94']);
  });
});

describe('extractPrefectureTemps', () => {
  const OBS = '2026-07-14T01:00:00+09:00';

  it('plucks the capital station when its temp is good', () => {
    const map = { '44132': { temp: [26.9, 0] } };
    const tokyo = extractPrefectureTemps(map, OBS).find(s => s.code === 13)!;
    expect(tokyo.tempC).toBe(26.9);
    expect(tokyo.obsTime).toBe(OBS);
  });

  it('treats a non-zero quality flag as missing and backfills from the same area', () => {
    const map = {
      '44132': { temp: [26.9, 2] },   // Tokyo capital station flagged suspect
      '44166': { temp: [25.4, 0] },   // another Tokyo-area station
    };
    const tokyo = extractPrefectureTemps(map, OBS).find(s => s.code === 13)!;
    expect(tokyo.tempC).toBe(25.4);
  });

  it('backfills Hokkaido across its 11–24 sub-area codes', () => {
    const map = { '12345': { temp: [19.5, 0] } }; // area 12, not Sapporo (14163)
    const hokkaido = extractPrefectureTemps(map, OBS).find(s => s.code === 1)!;
    expect(hokkaido.tempC).toBe(19.5);
  });

  it('returns null when nothing in the prefecture reports', () => {
    const okinawa = extractPrefectureTemps({}, OBS).find(s => s.code === 47)!;
    expect(okinawa.tempC).toBeNull();
  });
});

/* ─── provider: cache, fallback chain, single-flight ─────────────────── */

const LATEST = '2026-07-14T01:00:00+09:00';
const NEXT = '2026-07-14T01:10:00+09:00';

function fullMap(tempC: number): Record<string, unknown> {
  const m: Record<string, unknown> = {};
  for (const p of JAPAN_PREFECTURES) m[p.station] = { temp: [tempC, 0] };
  return m;
}

/** Scriptable fetch double: routes by substring, counts calls per route. */
function makeFetch(routes: Record<string, () => unknown | Promise<unknown>>) {
  const calls: Record<string, number> = {};
  const impl = async (url: string) => {
    const key = Object.keys(routes).find(k => url.includes(k));
    if (!key) throw new Error(`unrouted url: ${url}`);
    calls[key] = (calls[key] ?? 0) + 1;
    const body = await routes[key]!();
    if (body instanceof Error) throw body;
    return {
      ok: true, status: 200,
      text: async () => String(body),
      json: async () => body,
    };
  };
  return { impl, calls };
}

describe('createJapanTempsProvider', () => {
  it('happy path: serves JMA readings with source=jma', async () => {
    const { impl } = makeFetch({
      'latest_time.txt': () => LATEST,
      '/map/20260714010000.json': () => fullMap(24.5),
    });
    const p = createJapanTempsProvider({ fetchImpl: impl as any, now: () => 1_000 });
    const payload = await p.get();
    expect(payload.source).toBe('jma');
    expect(payload.stations.length).toBe(47);
    expect(payload.stations.find(s => s.code === 13)!.tempC).toBe(24.5);
    expect(payload.stations[0]!.obsTime).toBe(LATEST);
  });

  it('caches: no upstream hit at all inside the recheck window', async () => {
    const { impl, calls } = makeFetch({
      'latest_time.txt': () => LATEST,
      '/map/': () => fullMap(24.5),
    });
    let t = 1_000;
    const p = createJapanTempsProvider({ fetchImpl: impl as any, now: () => t });
    await p.get();
    t += RECHECK_MIN_MS - 1;
    await p.get();
    expect(calls['latest_time.txt']).toBe(1);
    expect(calls['/map/']).toBe(1);
  });

  it('rechecks after the window but refetches the map only on a NEW timestamp', async () => {
    let latest = LATEST;
    const { impl, calls } = makeFetch({
      'latest_time.txt': () => latest,
      '/map/': () => fullMap(25.0),
    });
    let t = 1_000;
    const p = createJapanTempsProvider({ fetchImpl: impl as any, now: () => t });
    await p.get();                       // fetch #1: latest + map
    t += RECHECK_MIN_MS + 1;
    await p.get();                       // fetch #2: latest only (same ts)
    expect(calls['latest_time.txt']).toBe(2);
    expect(calls['/map/']).toBe(1);
    latest = NEXT;
    t += RECHECK_MIN_MS + 1;
    await p.get();                       // fetch #3: latest + NEW map
    expect(calls['/map/']).toBe(2);
  });

  it('falls back to Open-Meteo when JMA is down', async () => {
    const { impl } = makeFetch({
      'jma.go.jp': () => new Error('jma down'),
      'open-meteo.com': () => JAPAN_PREFECTURES.map(() => ({
        current: { time: '2026-07-14T01:00', temperature_2m: 22.2 },
      })),
    });
    const p = createJapanTempsProvider({ fetchImpl: impl as any, now: () => 1_000 });
    const payload = await p.get();
    expect(payload.source).toBe('open-meteo');
    expect(payload.stations.length).toBe(47);
    expect(payload.stations[0]!.tempC).toBe(22.2);
  });

  it('falls back to simulation when every upstream is down', async () => {
    const { impl } = makeFetch({
      'jma.go.jp': () => new Error('down'),
      'open-meteo.com': () => new Error('down'),
    });
    const p = createJapanTempsProvider({
      fetchImpl: impl as any,
      now: () => Date.UTC(2026, 6, 14, 3, 0, 0),
    });
    const payload = await p.get();
    expect(payload.source).toBe('simulated');
    expect(payload.stations.length).toBe(47);
  });

  it('single-flight: concurrent gets share one upstream chain', async () => {
    let release!: () => void;
    const gate = new Promise<void>(r => { release = r; });
    const { impl, calls } = makeFetch({
      'latest_time.txt': async () => { await gate; return LATEST; },
      '/map/': () => fullMap(24.5),
    });
    const p = createJapanTempsProvider({ fetchImpl: impl as any, now: () => 1_000 });
    const a = p.get();
    const b = p.get();
    release();
    const [pa, pb] = await Promise.all([a, b]);
    expect(pa).toBe(pb);                     // same object, one refresh
    expect(calls['latest_time.txt']).toBe(1);
  });
});

/* ─── route wiring ───────────────────────────────────────────────────── */

describe('mock-join-server geoLive wiring', () => {
  it('createJoinServer default stays hermetic (simulated, no fetch)', async () => {
    const { createJoinServer } = await import('./mock-join-server');
    const request = (await import('supertest')).default;
    const s = createJoinServer({ jwtSecret: 'test', startRotationTimer: false });
    const res = await request(s.app).get('/api/v1/geo/japan-temps');
    expect(res.body.source).toBe('simulated');
    s.stopRotationTimer();
  });
});
