import { describe, it, expect } from 'vitest';
import GtfsRT from 'gtfs-realtime-bindings';
import {
  decodeVehiclePositions, createKumamotoVehiclesProvider, RT_CACHE_MS,
} from './kumamotoVehiclesAdapter';
import type { KumamotoNetwork, LonLat } from '../src/demo/kumamotoNetwork';

/* ─── synthetic protobuf helpers ─────────────────────────────────────── */

function encodeFeed(vehicles: Array<{
  id: string; lat: number; lon: number; bearing?: number; routeId?: string; ts?: number;
}>): Uint8Array {
  const msg = GtfsRT.transit_realtime.FeedMessage.fromObject({
    header: { gtfsRealtimeVersion: '2.0', incrementality: 0, timestamp: 1784100000 },
    entity: vehicles.map(v => ({
      id: v.id,
      vehicle: {
        trip: v.routeId ? { routeId: v.routeId } : undefined,
        vehicle: { id: v.id },
        position: { latitude: v.lat, longitude: v.lon, bearing: v.bearing ?? 0 },
        timestamp: v.ts ?? 1784100000,
      },
    })),
  });
  return GtfsRT.transit_realtime.FeedMessage.encode(msg).finish();
}

const emptyFeed = () => encodeFeed([]);

/* Test network — one bus route through the city core. */
const A: LonLat = [130.70, 32.80];
const C: LonLat = [130.71, 32.81];
const NET: KumamotoNetwork = {
  generated: '', source: 'test', bbox: [130.69, 32.79, 130.72, 32.82],
  operators: [{
    key: 'toshibus', kind: 'bus', nameJa: '都市バス', license: 'CC BY 4.0',
    routes: [{ id: 'r1', name: 'S1-0', lines: [[A, C]] }], stops: [],
  }],
};

function makeFetch(bodies: Record<string, () => Uint8Array | Error>) {
  const calls: Record<string, number> = {};
  const impl = async (url: string) => {
    const key = Object.keys(bodies).find(k => url.includes(k));
    if (!key) throw new Error(`unrouted: ${url}`);
    calls[key] = (calls[key] ?? 0) + 1;
    const body = bodies[key]!();
    if (body instanceof Error) throw body;
    return { ok: true, status: 200, arrayBuffer: async () => body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength) };
  };
  return { impl, calls };
}

const FEEDS = {
  toshibus: 'https://example.test/toshibus_vpos',
  kumabus: 'https://example.test/kumabus_vpos',
  dentetsu: 'https://example.test/dentetsu_vpos',
};

/* ─── decode ─────────────────────────────────────────────────────────── */

describe('decodeVehiclePositions', () => {
  it('maps entities to TransitVehicles', () => {
    const buf = encodeFeed([{ id: 'bus1', lat: 32.805, lon: 130.705, bearing: 123, routeId: 'S1' }]);
    const out = decodeVehiclePositions(buf, 'toshibus', new Map(), 1_000);
    expect(out.length).toBe(1);
    expect(out[0]).toMatchObject({
      id: 'toshibus-bus1', op: 'toshibus', routeId: 'S1',
      bearing: 123, ts: 1784100000_000,
    });
    // GTFS-RT positions are float32 — compare with tolerance.
    expect(out[0]!.lat).toBeCloseTo(32.805, 4);
    expect(out[0]!.lon).toBeCloseTo(130.705, 4);
  });

  it('derives a missing bearing from the previous fix', () => {
    const prev = new Map<string, { lat: number; lon: number; bearing: number }>();
    // First fix: no bearing in feed (0 = unset), no history → 0.
    const b1 = encodeFeed([{ id: 'v', lat: 32.800, lon: 130.700 }]);
    const r1 = decodeVehiclePositions(b1, 'toshibus', prev, 0);
    expect(r1[0]!.bearing).toBe(0);
    // Second fix ~1.1 km due north → bearing ≈ 0° (north); move east → ≈ 90°.
    const b2 = encodeFeed([{ id: 'v', lat: 32.800, lon: 130.710 }]);
    const r2 = decodeVehiclePositions(b2, 'toshibus', prev, 0);
    expect(r2[0]!.bearing).toBeCloseTo(90, 0);
  });

  it('skips entities without positions', () => {
    const msg = GtfsRT.transit_realtime.FeedMessage.fromObject({
      header: { gtfsRealtimeVersion: '2.0', timestamp: 1 },
      entity: [{ id: 'x', vehicle: { vehicle: { id: 'x' } } }],
    });
    const buf = GtfsRT.transit_realtime.FeedMessage.encode(msg).finish();
    expect(decodeVehiclePositions(buf, 'toshibus', new Map(), 0).length).toBe(0);
  });
});

/* ─── provider ───────────────────────────────────────────────────────── */

describe('createKumamotoVehiclesProvider', () => {
  it('live path: merges feeds, filters to the map bbox, source=gtfs-rt', async () => {
    const { impl } = makeFetch({
      toshibus_vpos: () => encodeFeed([
        { id: 'a', lat: 32.805, lon: 130.705, bearing: 90 },   // on-map
        { id: 'far', lat: 33.5, lon: 131.5, bearing: 90 },     // off-map — dropped
      ]),
      kumabus_vpos: () => encodeFeed([{ id: 'b', lat: 32.81, lon: 130.71, bearing: 10 }]),
      dentetsu_vpos: emptyFeed,
    });
    const p = createKumamotoVehiclesProvider({ network: () => NET, fetchImpl: impl as any, now: () => 1_000, feeds: FEEDS });
    const payload = await p.get();
    expect(payload.source).toBe('gtfs-rt');
    expect(payload.vehicles.map(v => v.id).sort()).toEqual(['kumabus-b', 'toshibus-a']);
  });

  it('caches live results for the 15 s window (one sweep)', async () => {
    const { impl, calls } = makeFetch({
      toshibus_vpos: () => encodeFeed([{ id: 'a', lat: 32.805, lon: 130.705 }]),
      kumabus_vpos: emptyFeed,
      dentetsu_vpos: emptyFeed,
    });
    let t = 1_000;
    const p = createKumamotoVehiclesProvider({ network: () => NET, fetchImpl: impl as any, now: () => t, feeds: FEEDS });
    await p.get();
    t += RT_CACHE_MS - 1;
    await p.get();
    expect(calls['toshibus_vpos']).toBe(1);
    t += 2;
    await p.get();
    expect(calls['toshibus_vpos']).toBe(2);
  });

  it('empty feeds (off-hours) → simulated on the real network', async () => {
    const { impl } = makeFetch({
      toshibus_vpos: emptyFeed, kumabus_vpos: emptyFeed, dentetsu_vpos: emptyFeed,
    });
    const p = createKumamotoVehiclesProvider({ network: () => NET, fetchImpl: impl as any, now: () => Date.UTC(2026, 6, 15, 3, 0, 0), feeds: FEEDS });
    const payload = await p.get();
    expect(payload.source).toBe('simulated');
    expect(payload.vehicles.length).toBeGreaterThan(0);
    expect(payload.vehicles.every(v => v.op === 'toshibus')).toBe(true);
  });

  it('all feeds unreachable → simulated', async () => {
    const { impl } = makeFetch({
      toshibus_vpos: () => new Error('down'),
      kumabus_vpos: () => new Error('down'),
      dentetsu_vpos: () => new Error('down'),
    });
    const p = createKumamotoVehiclesProvider({ network: () => NET, fetchImpl: impl as any, now: () => Date.UTC(2026, 6, 15, 3, 0, 0), feeds: FEEDS });
    const payload = await p.get();
    expect(payload.source).toBe('simulated');
    expect(payload.vehicles.length).toBeGreaterThan(0);
  });

  it('cached simulated frames keep advancing inside the window', async () => {
    const { impl } = makeFetch({
      toshibus_vpos: emptyFeed, kumabus_vpos: emptyFeed, dentetsu_vpos: emptyFeed,
    });
    let t = Date.UTC(2026, 6, 15, 3, 0, 0);
    const p = createKumamotoVehiclesProvider({ network: () => NET, fetchImpl: impl as any, now: () => t, feeds: FEEDS });
    const a = await p.get();
    t += 5_000; // still inside the cache window
    const b = await p.get();
    const moved = a.vehicles.some((v, i) => v.lat !== b.vehicles[i]!.lat || v.lon !== b.vehicles[i]!.lon);
    expect(moved).toBe(true);
  });

  it('single-flight: concurrent gets share one sweep', async () => {
    let release!: () => void;
    const gate = new Promise<void>(r => { release = r; });
    const buf = encodeFeed([{ id: 'a', lat: 32.805, lon: 130.705 }]);
    let calls = 0;
    const impl = async () => {
      calls++;
      await gate;
      return { ok: true, status: 200, arrayBuffer: async () => buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength) };
    };
    const p = createKumamotoVehiclesProvider({ network: () => NET, fetchImpl: impl as any, now: () => 1_000, feeds: FEEDS });
    const a = p.get();
    const b = p.get();
    release();
    const [pa, pb] = await Promise.all([a, b]);
    expect(pa).toBe(pb);
    expect(calls).toBe(3); // one sweep = one fetch per feed, not per caller
  });
});
