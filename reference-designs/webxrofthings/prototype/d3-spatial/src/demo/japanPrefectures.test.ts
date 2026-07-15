import { describe, it, expect } from 'vitest';
import {
  JAPAN_PREFECTURES, JAPAN_GRID_COLS, JAPAN_GRID_ROWS,
  simulatedJapanTemps,
} from './japanPrefectures';

describe('japanPrefectures table', () => {
  it('has exactly 47 prefectures with JIS codes 1..47', () => {
    expect(JAPAN_PREFECTURES.length).toBe(47);
    const codes = JAPAN_PREFECTURES.map(p => p.code).sort((a, b) => a - b);
    expect(codes).toEqual(Array.from({ length: 47 }, (_, i) => i + 1));
  });

  it('tile-grid cells are unique and within bounds', () => {
    const seen = new Set<string>();
    for (const p of JAPAN_PREFECTURES) {
      const key = `${p.gridX},${p.gridY}`;
      expect(seen.has(key), `duplicate grid cell ${key} (${p.nameEn})`).toBe(false);
      seen.add(key);
      expect(p.gridX).toBeGreaterThanOrEqual(0);
      expect(p.gridX).toBeLessThan(JAPAN_GRID_COLS);
      expect(p.gridY).toBeGreaterThanOrEqual(0);
      expect(p.gridY).toBeLessThan(JAPAN_GRID_ROWS);
    }
  });

  it('AMeDAS station IDs are unique 5-digit strings', () => {
    const ids = new Set<string>();
    for (const p of JAPAN_PREFECTURES) {
      expect(p.station).toMatch(/^\d{5}$/);
      ids.add(p.station);
    }
    expect(ids.size).toBe(47);
  });

  it('grid layout is geographically sane at the anchors', () => {
    const byName = new Map(JAPAN_PREFECTURES.map(p => [p.nameEn, p]));
    const hokkaido = byName.get('Hokkaido')!;
    const okinawa = byName.get('Okinawa')!;
    const tokyo = byName.get('Tokyo')!;
    const kanagawa = byName.get('Kanagawa')!;
    // Hokkaido is the far north-east tile; Okinawa the far south-west.
    expect(hokkaido.gridY).toBe(0);
    expect(okinawa.gridY).toBe(JAPAN_GRID_ROWS - 1);
    expect(okinawa.gridX).toBeLessThan(hokkaido.gridX);
    // Kanagawa sits directly south of Tokyo.
    expect(kanagawa.gridX).toBe(tokyo.gridX);
    expect(kanagawa.gridY).toBe(tokyo.gridY + 1);
  });
});

describe('simulatedJapanTemps', () => {
  it('is deterministic for a fixed timestamp and covers all 47', () => {
    const t = Date.UTC(2026, 6, 14, 3, 0, 0); // 12:00 JST
    const a = simulatedJapanTemps(t);
    const b = simulatedJapanTemps(t);
    expect(a).toEqual(b);
    expect(a.source).toBe('simulated');
    expect(a.stations.length).toBe(47);
  });

  it('produces plausible July temperatures, Hokkaido cooler than Okinawa', () => {
    const t = Date.UTC(2026, 6, 14, 5, 0, 0); // 14:00 JST — daily peak
    const { stations } = simulatedJapanTemps(t);
    for (const s of stations) {
      expect(s.tempC).not.toBeNull();
      expect(s.tempC!).toBeGreaterThan(10);
      expect(s.tempC!).toBeLessThan(40);
    }
    const hokkaido = stations.find(s => s.code === 1)!;
    const okinawa = stations.find(s => s.code === 47)!;
    expect(hokkaido.tempC!).toBeLessThan(okinawa.tempC!);
  });

  it('drifts over time — two polls 5 minutes apart differ somewhere', () => {
    const t = Date.UTC(2026, 6, 14, 3, 0, 0);
    const a = simulatedJapanTemps(t);
    const b = simulatedJapanTemps(t + 5 * 60_000);
    const moved = a.stations.some((s, i) => s.tempC !== b.stations[i]!.tempC);
    expect(moved).toBe(true);
  });
});
