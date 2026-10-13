/**
 * UC5 Phase-2 live adapter — Japan prefecture temperatures.
 *
 * Chain (docs/uc5-uc6-geo-dataspaces.md §1.1, §3.1):
 *   1. JMA AMeDAS bosai JSON (primary — official 10-minutely station obs,
 *      no key): GET latest_time.txt → GET map/{yyyyMMddHHmmss}.json
 *      (~300 KB, all 1,286 stations) → pluck the 47 capital stations.
 *   2. Open-Meteo (fallback — model analysis, no key): one request with
 *      all 47 capital lat/lons. Free tier counts a 47-location call as
 *      47 requests → hard 10-minute floor between upstream hits.
 *   3. simulatedJapanTemps (final fallback — never fails).
 *
 * Rate discipline:
 *   - latest_time.txt is re-checked at most once per RECHECK_MIN_MS; the
 *     map JSON is re-fetched only when the timestamp actually advanced
 *     (JMA publishes every 10 min, so ≤6 map fetches/hour regardless of
 *     how many clients poll us).
 *   - Concurrent requests share one in-flight refresh (single-flight),
 *     so a headset fleet can't fan out to JMA.
 *
 * Every value in the AMeDAS map JSON is `[value, qualityFlag]`; flag 0 is
 * good, anything else is suspect/missing → treated as null, then backfilled
 * from any same-area station (first 2 digits of the station ID are the JMA
 * area code; Hokkaido spans 11–24, Okinawa 91–94).
 */

import {
  JAPAN_PREFECTURES, simulatedJapanTemps,
  type JapanTempsPayload, type PrefectureTemp,
} from '../src/demo/japanPrefectures';

const JMA_LATEST = 'https://www.jma.go.jp/bosai/amedas/data/latest_time.txt';
const JMA_MAP = (ts: string) => `https://www.jma.go.jp/bosai/amedas/data/map/${ts}.json`;
const OPEN_METEO = 'https://api.open-meteo.com/v1/forecast';

export const RECHECK_MIN_MS = 60_000;
export const OPEN_METEO_MIN_MS = 600_000;
const UPSTREAM_TIMEOUT_MS = 8_000;

type FetchLike = (url: string, init?: { signal?: AbortSignal }) => Promise<{
  ok: boolean; status: number;
  text(): Promise<string>; json(): Promise<unknown>;
}>;

export interface JapanTempsProviderOpts {
  /** Injectable for tests. Defaults to global fetch. */
  fetchImpl?: FetchLike;
  /** Injectable clock for tests. Defaults to Date.now. */
  now?: () => number;
}

/** '2026-07-14T01:00:00+09:00' → '20260714010000' (JMA renders JST). */
export function jmaTimestampToPath(isoJst: string): string | null {
  const m = isoJst.trim().match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})/);
  if (!m) return null;
  return `${m[1]}${m[2]}${m[3]}${m[4]}${m[5]}${m[6]}`;
}

function tempFrom(entry: unknown): number | null {
  const t = (entry as { temp?: unknown })?.temp;
  if (!Array.isArray(t) || typeof t[0] !== 'number') return null;
  if (t[1] !== 0) return null; // quality flag — non-zero = suspect/missing
  return t[0];
}

/** Station-ID prefixes acceptable as same-prefecture backfill. */
export function prefixesFor(code: number, station: string): string[] {
  if (code === 1) return Array.from({ length: 14 }, (_, i) => String(11 + i)); // Hokkaido 11–24
  if (code === 47) return ['91', '92', '93', '94'];                            // Okinawa groups
  return [station.slice(0, 2)];
}

/** Pluck one reading per prefecture from a full AMeDAS map snapshot. */
export function extractPrefectureTemps(
  mapJson: Record<string, unknown>,
  obsTime: string,
): PrefectureTemp[] {
  return JAPAN_PREFECTURES.map((p) => {
    let tempC = tempFrom(mapJson[p.station]);
    if (tempC == null) {
      for (const prefix of prefixesFor(p.code, p.station)) {
        for (const [id, entry] of Object.entries(mapJson)) {
          if (!id.startsWith(prefix)) continue;
          const t = tempFrom(entry);
          if (t != null) { tempC = t; break; }
        }
        if (tempC != null) break;
      }
    }
    return { code: p.code, nameEn: p.nameEn, nameJa: p.nameJa, tempC, obsTime };
  });
}

export interface JapanTempsProvider {
  /** Never rejects — worst case returns the simulated payload. */
  get(): Promise<JapanTempsPayload>;
}

export function createJapanTempsProvider(opts: JapanTempsProviderOpts = {}): JapanTempsProvider {
  const fetchImpl: FetchLike = opts.fetchImpl ?? (fetch as unknown as FetchLike);
  const now = opts.now ?? Date.now;

  let cached: { payload: JapanTempsPayload; jmaTs: string; atMs: number } | null = null;
  let cachedOpenMeteo: { payload: JapanTempsPayload; atMs: number } | null = null;
  let inflight: Promise<JapanTempsPayload> | null = null;
  let lastLoggedSource: string | null = null;

  async function timedFetch(url: string) {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), UPSTREAM_TIMEOUT_MS);
    try {
      const resp = await fetchImpl(url, { signal: ctrl.signal });
      if (!resp.ok) throw new Error(`${url} → HTTP ${resp.status}`);
      return resp;
    } finally {
      clearTimeout(timer);
    }
  }

  function logSource(source: string, detail: string) {
    if (source !== lastLoggedSource) {
      console.log(`[japan-temps] source → ${source} (${detail})`);
      lastLoggedSource = source;
    }
  }

  async function fromJma(): Promise<JapanTempsPayload> {
    const latestIso = (await (await timedFetch(JMA_LATEST)).text()).trim();
    const ts = jmaTimestampToPath(latestIso);
    if (!ts) throw new Error(`unparseable latest_time: "${latestIso.slice(0, 40)}"`);

    // Timestamp unchanged → the 10-min slot hasn't advanced; reuse the map.
    if (cached && cached.jmaTs === ts) {
      cached.atMs = now();
      return cached.payload;
    }

    const mapJson = await (await timedFetch(JMA_MAP(ts))).json() as Record<string, unknown>;
    const stations = extractPrefectureTemps(mapJson, latestIso);
    const covered = stations.filter(s => s.tempC != null).length;
    const payload: JapanTempsPayload = {
      updated: new Date(now()).toISOString(),
      source: 'jma',
      stations,
    };
    cached = { payload, jmaTs: ts, atMs: now() };
    logSource('jma', `obs ${latestIso}, ${covered}/47 stations reporting`);
    return payload;
  }

  async function fromOpenMeteo(): Promise<JapanTempsPayload> {
    // Respect the free-tier budget: reuse the last model analysis for 10 min.
    if (cachedOpenMeteo && now() - cachedOpenMeteo.atMs < OPEN_METEO_MIN_MS) {
      return cachedOpenMeteo.payload;
    }
    const lats = JAPAN_PREFECTURES.map(p => p.lat).join(',');
    const lons = JAPAN_PREFECTURES.map(p => p.lon).join(',');
    const url = `${OPEN_METEO}?latitude=${lats}&longitude=${lons}&current=temperature_2m&timezone=Asia%2FTokyo`;
    const body = await (await timedFetch(url)).json() as
      Array<{ current?: { time?: string; temperature_2m?: number } }> |
      { current?: { time?: string; temperature_2m?: number } };
    const rows = Array.isArray(body) ? body : [body];
    if (rows.length !== JAPAN_PREFECTURES.length) {
      throw new Error(`open-meteo returned ${rows.length} locations (want 47)`);
    }
    const stations: PrefectureTemp[] = JAPAN_PREFECTURES.map((p, i) => ({
      code: p.code, nameEn: p.nameEn, nameJa: p.nameJa,
      tempC: typeof rows[i]?.current?.temperature_2m === 'number' ? rows[i]!.current!.temperature_2m! : null,
      obsTime: rows[i]?.current?.time ?? new Date(now()).toISOString(),
    }));
    const payload: JapanTempsPayload = {
      updated: new Date(now()).toISOString(),
      source: 'open-meteo',
      stations,
    };
    cachedOpenMeteo = { payload, atMs: now() };
    logSource('open-meteo', 'JMA unavailable — model analysis');
    return payload;
  }

  async function refresh(): Promise<JapanTempsPayload> {
    try {
      return await fromJma();
    } catch (jmaErr) {
      console.warn(`[japan-temps] JMA fetch failed: ${(jmaErr as Error).message}`);
      try {
        return await fromOpenMeteo();
      } catch (omErr) {
        console.warn(`[japan-temps] Open-Meteo fetch failed: ${(omErr as Error).message}`);
        logSource('simulated', 'all upstreams unavailable');
        return simulatedJapanTemps(now());
      }
    }
  }

  return {
    async get() {
      // Fresh-enough cache — serve without touching upstream at all.
      if (cached && now() - cached.atMs < RECHECK_MIN_MS) return cached.payload;
      if (inflight) return inflight;
      inflight = refresh().finally(() => { inflight = null; });
      return inflight;
    },
  };
}
