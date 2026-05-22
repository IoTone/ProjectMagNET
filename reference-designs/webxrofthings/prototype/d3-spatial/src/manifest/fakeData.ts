/**
 * fakeData — synthesised stand-ins for offline manifest sensors.
 *
 * When a URL-source mark goes offline (see `healthMonitor.ts`), the
 * loader's setInterval calls into this module to fabricate a plausible
 * payload in the shape the cell builder expects. The cell sees the
 * same JSON it'd see from a real device — no per-cell branching
 * required — and the user sees moving data on the chart with a
 * yellow "DEMO MODE" HUD warning them that the values aren't real.
 *
 * Synthesised waveforms are deterministic per `(markId, time)` — a
 * sine of a slow period plus harmonics — so each mark is recognisably
 * its own signal, and successive ticks line up smoothly with the
 * previous (no jumps when the cell rebuilds from the new payload).
 *
 * Shapes supported here are the ones UC1 actually uses (`series` for
 * line + arc, `distributions` for streamgraph). Other shapes
 * (`hierarchy`, `graph`, `imu`, `video`) return null — the loader
 * leaves those alone and the user's chart freezes (acceptable; UC1
 * is the priority and these shapes don't appear on any current
 * sensor that would realistically go offline).
 */

import type { MarkSpec } from './schema';

/* Pure hash → number in [0, 1). Same `markId` always yields the same
 * seed, so two sessions of the same dataspace produce identical fake
 * patterns and a screenshot from one comparison still matches. */
function seed(markId: string): number {
  let h = 2166136261;
  for (let i = 0; i < markId.length; i++) {
    h ^= markId.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return ((h >>> 0) % 100000) / 100000;
}

/** Pick a (centre, amplitude) for a series fake.
 *  Preference order:
 *    1. Mark's own `vMin` / `vMax` (UC2's pollen at 0-12, AQI at
 *       0-150, barometer at 1005-1025, body-temp at 36-37.5 …).
 *       Centre = midpoint, amplitude = 40 % of span — leaves room
 *       above and below so the line doesn't crowd the chart border.
 *    2. Id-keyword heuristic for marks with no `vMin`/`vMax`
 *       (UC1's hr-line / br-line / phases auto-fit, but their ids
 *       still suggest sensible ranges).
 *    3. Generic 0-100 centred at 50, ±40. Reserved for genuinely
 *       unknown marks — should never fire on a real UC manifest. */
function fakeRangeFor(spec: MarkSpec): { center: number; amplitude: number } {
  const cfg = (spec.config ?? {}) as { vMin?: number; vMax?: number };
  if (typeof cfg.vMin === 'number' && typeof cfg.vMax === 'number' && cfg.vMax > cfg.vMin) {
    const span = cfg.vMax - cfg.vMin;
    return { center: cfg.vMin + span * 0.5, amplitude: span * 0.40 };
  }
  const id = spec.id.toLowerCase();
  if (id.includes('hr')      || id.includes('heart'))   return { center: 70,   amplitude: 10  };
  if (id.includes('br')      || id.includes('breath'))  return { center: 14,   amplitude: 2   };
  if (id.includes('temp'))                              return { center: 36.6, amplitude: 0.3 };
  if (id.includes('lux')     || id.includes('light'))   return { center: 200,  amplitude: 50  };
  if (id.includes('pollen'))                            return { center: 6,    amplitude: 2.4 };
  if (id.includes('aqi')     || id.includes('air-q'))   return { center: 50,   amplitude: 20  };
  if (id.includes('baro')    || id.includes('press'))   return { center: 1015, amplitude: 8   };
  return { center: 50, amplitude: 40 };
}

/** Series shape: 60 samples over the last 60 min, 1/min. The latest
 *  sample matches a steadily-incrementing wall clock so chart auto-fit
 *  and the arc-gauge "read latest sample" path both work. Values stay
 *  inside the mark's [vMin, vMax] band whenever it's declared, so a
 *  narrow-range chart (pollen 0-12) doesn't get a 0-100 generic sine
 *  scrawled across the whole scene. */
function fakeSeries(spec: MarkSpec): { samples: Array<{ t: number; v: number }> } {
  const s = seed(spec.id);
  const { center, amplitude } = fakeRangeFor(spec);
  const nowMs = Date.now();
  const samples: Array<{ t: number; v: number }> = [];
  for (let i = 59; i >= 0; i--) {
    const t = nowMs - i * 60_000;            // 1 min spacing
    const tMin = (nowMs / 60_000) - i;       // minutes of wall clock — drives the waveform
    /* Main slow sine + small higher-frequency harmonic for the
     * "kind of jittery, kind of trending" look real sensors have. */
    const v = center
            + amplitude       * Math.sin(tMin * 0.07 + s * 6.28)
            + amplitude * 0.2 * Math.sin(tMin * 0.5);
    samples.push({ t, v });
  }
  return { samples };
}

/** Distributions shape: streamgraph categories. Three sine waves
 *  90° out of phase so the streamgraph reads as a coupled heart /
 *  breath / total composite — same visual language as the real
 *  /api/v1/vitals/phases endpoint. */
function fakeDistributions(markId: string): number[][] {
  const s = seed(markId);
  const tNow = Date.now() / 1000;
  const heart  = Array.from({ length: 120 }, (_, i) => {
    const t = tNow - (120 - i) * 0.5;
    return 0.5 + 0.4 * Math.sin(t * 1.0 + s * 6.28);
  });
  const breath = Array.from({ length: 120 }, (_, i) => {
    const t = tNow - (120 - i) * 0.5;
    return 0.5 + 0.3 * Math.sin(t * 0.18 + s * 6.28 + Math.PI / 2);
  });
  const total  = Array.from({ length: 120 }, (_, i) => heart[i]! * 0.6 + breath[i]! * 0.4);
  return [heart, breath, total];
}

/** Top-level entry. Returns the JSON-shaped fake payload, or null if
 *  we don't know how to fake this shape (caller leaves the spec alone
 *  and the chart stops updating). */
export function generateFakePayload(spec: MarkSpec, shape: string): unknown | null {
  switch (shape) {
    case 'series':        return fakeSeries(spec);
    case 'distributions': return fakeDistributions(spec.id);
    default:              return null;
  }
}
