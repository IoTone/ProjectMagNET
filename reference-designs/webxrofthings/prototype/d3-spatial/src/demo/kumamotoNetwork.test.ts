import { describe, it, expect } from 'vitest';
import {
  makeCityProjection, lineLengthMeters, pointAlong, simulatedKumamotoVehicles,
  buildRouteNameIndex,
  type KumamotoNetwork, type LonLat,
} from './kumamotoNetwork';

/* Kumamoto-ish coordinates for realistic cos(lat) behavior. */
const A: LonLat = [130.70, 32.80];
const B: LonLat = [130.71, 32.80]; // ~940 m east of A at this latitude
const C: LonLat = [130.71, 32.81]; // ~1113 m north of B

describe('makeCityProjection', () => {
  const proj = makeCityProjection([130.70, 32.80, 130.72, 32.81], 1.0);

  it('centers the bbox at the origin', () => {
    const c = proj.toXY(130.71, 32.805);
    expect(Math.abs(c.x)).toBeLessThan(1e-9);
    expect(Math.abs(c.y)).toBeLessThan(1e-9);
  });

  it('fits the longer side to the requested width', () => {
    // Lon span 0.02° × cos(32.805°) ≈ 0.0168 "units"; lat span 0.01°.
    // Longer side is X → width 1.0, height proportionally smaller.
    expect(proj.width).toBeCloseTo(1.0, 6);
    expect(proj.height).toBeGreaterThan(0.5);
    expect(proj.height).toBeLessThan(0.7);
  });

  it('is east-positive / north-positive', () => {
    const west = proj.toXY(130.70, 32.805);
    const east = proj.toXY(130.72, 32.805);
    const south = proj.toXY(130.71, 32.80);
    const north = proj.toXY(130.71, 32.81);
    expect(east.x).toBeGreaterThan(west.x);
    expect(north.y).toBeGreaterThan(south.y);
  });
});

describe('polyline walking', () => {
  it('measures a ~940 m east segment at Kumamoto latitude', () => {
    const len = lineLengthMeters([A, B]);
    expect(len).toBeGreaterThan(900);
    expect(len).toBeLessThan(980);
  });

  it('pointAlong interpolates position and bearing', () => {
    const half = lineLengthMeters([A, B]) / 2;
    const p = pointAlong([A, B], half);
    expect(p.lon).toBeCloseTo(130.705, 3);
    expect(p.lat).toBeCloseTo(32.80, 6);
    expect(p.bearing).toBeCloseTo(90, 0); // heading due east
  });

  it('clamps past the end and reports the final bearing', () => {
    const p = pointAlong([B, C], 1e9);
    expect(p.lon).toBeCloseTo(C[0], 9);
    expect(p.lat).toBeCloseTo(C[1], 9);
    expect(p.bearing).toBeCloseTo(0, 0); // due north
  });
});

describe('simulatedKumamotoVehicles', () => {
  const net: KumamotoNetwork = {
    generated: '', source: 'test', bbox: [130.70, 32.80, 130.72, 32.81],
    operators: [
      {
        key: 'tram', kind: 'tram', nameJa: '市電', license: 'CC BY 2.1 JP',
        routes: [{ id: 't1', name: 'A', lines: [[A, B, C]] }], stops: [],
      },
      {
        key: 'toshibus', kind: 'bus', nameJa: '都市バス', license: 'CC BY 4.0',
        routes: [
          { id: 'r1', name: 'S1-0', lines: [[A, B, C]] },
          { id: 'r2', name: 'S2-0', lines: [[A, C]] },
          { id: 'tiny', name: 'X', lines: [[A, [130.7001, 32.80]]] }, // < 500 m — excluded
        ],
        stops: [A, B],
      },
    ],
  };

  it('is deterministic and simulates buses only (tram is static)', () => {
    const t = Date.UTC(2026, 6, 14, 3, 0, 0);
    const a = simulatedKumamotoVehicles(net, t);
    const b = simulatedKumamotoVehicles(net, t);
    expect(a).toEqual(b);
    expect(a.source).toBe('simulated');
    expect(a.vehicles.length).toBe(4); // 2 eligible routes × 2 vehicles
    expect(a.vehicles.every(v => v.op === 'toshibus')).toBe(true);
  });

  it('keeps vehicles on the network (within the route bbox)', () => {
    const { vehicles } = simulatedKumamotoVehicles(net, Date.UTC(2026, 6, 14, 5, 0, 0));
    for (const v of vehicles) {
      expect(v.lon).toBeGreaterThanOrEqual(130.70 - 1e-6);
      expect(v.lon).toBeLessThanOrEqual(130.72 + 1e-6);
      expect(v.lat).toBeGreaterThanOrEqual(32.80 - 1e-6);
      expect(v.lat).toBeLessThanOrEqual(32.81 + 1e-6);
      expect(v.bearing).toBeGreaterThanOrEqual(0);
      expect(v.bearing).toBeLessThan(360);
    }
  });

  it('buildRouteNameIndex maps every variant id (and the primary) to the line name', () => {
    const netWithIds: KumamotoNetwork = {
      ...net,
      operators: [{
        ...net.operators[1]!,
        routes: [
          { id: 'r1', name: 'S1-0', lines: [[A, C]], ids: ['r1', 'r1b', 'r1c'] },
          { id: 'r9', name: 'K2-0', lines: [[A, C]] }, // no ids array — falls back to id
        ],
      }],
    };
    const idx = buildRouteNameIndex(netWithIds);
    expect(idx.get('r1')).toBe('S1-0');
    expect(idx.get('r1b')).toBe('S1-0');
    expect(idx.get('r1c')).toBe('S1-0');
    expect(idx.get('r9')).toBe('K2-0');
    expect(idx.get('unknown')).toBeUndefined();
  });

  it('moves vehicles between polls 15 s apart', () => {
    const t = Date.UTC(2026, 6, 14, 3, 0, 0);
    const a = simulatedKumamotoVehicles(net, t);
    const b = simulatedKumamotoVehicles(net, t + 15_000);
    const moved = a.vehicles.some((v, i) =>
      v.lat !== b.vehicles[i]!.lat || v.lon !== b.vehicles[i]!.lon);
    expect(moved).toBe(true);
  });
});
