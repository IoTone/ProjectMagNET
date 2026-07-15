/**
 * UC6 Phase-4 live adapter — Kumamoto bus positions via GTFS-RT.
 *
 * Upstream: the joint bus operators' Bus-Vision open-data feeds
 * (km.bus-vision.jp, CC BY 4.0, no auth) — VehiclePosition protobuf per
 * operator, updated every 15 s. The publisher explicitly asks not to poll
 * faster, so this adapter enforces:
 *   - a 15 s cache floor (one upstream sweep per window no matter how many
 *     headsets poll us),
 *   - single-flight (concurrent requests share one in-flight sweep),
 *   - one fetch per operator per sweep, in parallel.
 *
 * Fallback: outside service hours (~06:00–23:00 JST) the feeds return a
 * header-only FeedMessage with zero entities; fetch failures look the same
 * from the caller's side. Either way this adapter falls back to the
 * deterministic simulation walking the committed network snapshot, flagged
 * `source: 'simulated'` so the scene badge tells the truth.
 *
 * Bearing: GTFS-RT `position.bearing` is optional. When absent we derive it
 * from the vehicle's previous fix (kept in a small per-vehicle memory) and
 * fall back to 0 for a first sighting.
 */

import GtfsRT from 'gtfs-realtime-bindings';
import {
  simulatedKumamotoVehicles, lineLengthMeters, buildRouteNameIndex,
  type KumamotoNetwork, type KumamotoVehiclesPayload, type TransitVehicle,
} from '../src/demo/kumamotoNetwork';

export const RT_CACHE_MS = 15_000;
const UPSTREAM_TIMEOUT_MS = 8_000;

/** Operator key (ours / snapshot) → Bus-Vision feed slug. */
export const RT_FEEDS: Record<string, string> = {
  toshibus: 'https://km.bus-vision.jp/realtime/toshibus_vpos_update.bin',
  kumabus: 'https://km.bus-vision.jp/realtime/kumabus_vpos_update.bin',
  dentetsu: 'https://km.bus-vision.jp/realtime/dentetsu_vpos_update.bin',
};

type FetchLike = (url: string, init?: { signal?: AbortSignal }) => Promise<{
  ok: boolean; status: number;
  arrayBuffer(): Promise<ArrayBuffer>;
}>;

export interface KumamotoVehiclesProviderOpts {
  /** Supplies the network snapshot (for the simulation fallback + bbox filter). */
  network: () => KumamotoNetwork | null;
  fetchImpl?: FetchLike;
  now?: () => number;
  feeds?: Record<string, string>;
}

export interface KumamotoVehiclesProvider {
  /** Never rejects — worst case returns the simulated payload. */
  get(): Promise<KumamotoVehiclesPayload>;
}

/** Decode one VehiclePosition feed into TransitVehicles. Exported for tests. */
export function decodeVehiclePositions(
  buf: Uint8Array,
  op: string,
  prevFixes: Map<string, { lat: number; lon: number; bearing: number }>,
  nowMs: number,
): TransitVehicle[] {
  const msg = GtfsRT.transit_realtime.FeedMessage.decode(buf);
  const out: TransitVehicle[] = [];
  for (const entity of msg.entity) {
    const v = entity.vehicle;
    const pos = v?.position;
    if (!v || !pos || typeof pos.latitude !== 'number' || typeof pos.longitude !== 'number') continue;
    const id = `${op}-${v.vehicle?.id ?? entity.id}`;

    // Bearing: feed value, else derived from the previous fix, else 0.
    let bearing = (typeof pos.bearing === 'number' && pos.bearing !== 0) ? pos.bearing : NaN;
    const prev = prevFixes.get(id);
    if (!Number.isFinite(bearing) && prev) {
      const kx = Math.cos((pos.latitude * Math.PI) / 180);
      const dx = (pos.longitude - prev.lon) * kx;
      const dy = pos.latitude - prev.lat;
      // Only re-derive when the vehicle actually moved (~>5 m).
      bearing = Math.hypot(dx, dy) * 111_320 > 5
        ? ((Math.atan2(dx, dy) * 180) / Math.PI + 360) % 360
        : prev.bearing;
    }
    if (!Number.isFinite(bearing)) bearing = 0;
    prevFixes.set(id, { lat: pos.latitude, lon: pos.longitude, bearing });

    const tsSec = Number(v.timestamp ?? msg.header.timestamp ?? 0);
    out.push({
      id,
      op,
      routeId: v.trip?.routeId ?? '',
      routeName: v.trip?.routeId ?? '',
      lat: pos.latitude,
      lon: pos.longitude,
      bearing,
      ts: tsSec > 0 ? tsSec * 1000 : nowMs,
    });
  }
  return out;
}

export function createKumamotoVehiclesProvider(opts: KumamotoVehiclesProviderOpts): KumamotoVehiclesProvider {
  const fetchImpl: FetchLike = opts.fetchImpl ?? (fetch as unknown as FetchLike);
  const now = opts.now ?? Date.now;
  const feeds = opts.feeds ?? RT_FEEDS;

  let cached: { payload: KumamotoVehiclesPayload; atMs: number } | null = null;
  let inflight: Promise<KumamotoVehiclesPayload> | null = null;
  const prevFixes = new Map<string, { lat: number; lon: number; bearing: number }>();
  let lastLoggedSource: string | null = null;
  let routeNames: Map<string, string> | null = null;
  function routeNameFor(routeId: string): string {
    if (!routeNames) {
      const net = opts.network();
      routeNames = net ? buildRouteNameIndex(net) : new Map();
    }
    return routeNames.get(routeId) ?? routeId;
  }

  function logSource(source: string, detail: string) {
    if (source !== lastLoggedSource) {
      console.log(`[kumamoto-vehicles] source → ${source} (${detail})`);
      lastLoggedSource = source;
    }
  }

  function simulated(): KumamotoVehiclesPayload {
    const net = opts.network();
    if (!net) return { updated: new Date(now()).toISOString(), source: 'simulated', vehicles: [] };
    return simulatedKumamotoVehicles(net, now());
  }

  async function fetchFeed(op: string, url: string): Promise<TransitVehicle[]> {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), UPSTREAM_TIMEOUT_MS);
    try {
      const resp = await fetchImpl(url, { signal: ctrl.signal });
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      return decodeVehiclePositions(new Uint8Array(await resp.arrayBuffer()), op, prevFixes, now());
    } finally {
      clearTimeout(timer);
    }
  }

  async function sweep(): Promise<KumamotoVehiclesPayload> {
    const results = await Promise.allSettled(
      Object.entries(feeds).map(([op, url]) => fetchFeed(op, url)),
    );
    const vehicles: TransitVehicle[] = [];
    let okFeeds = 0;
    for (const r of results) {
      if (r.status === 'fulfilled') { okFeeds++; vehicles.push(...r.value); }
    }
    // Live trip.routeIds → display line names via the snapshot's variant-id
    // index (the decode step fills routeName with the raw routeId).
    for (const v of vehicles) v.routeName = routeNameFor(v.routeId);

    // Keep only vehicles on (or near) the rendered city core — the scene's
    // plate is clipped to the snapshot bbox, and a far-suburban bus would
    // float off the edge of the map.
    const net = opts.network();
    let onMap = vehicles;
    if (net) {
      const [minLon, minLat, maxLon, maxLat] = net.bbox;
      const M = 0.005; // ~500 m of grace at the edges
      onMap = vehicles.filter(v =>
        v.lon >= minLon - M && v.lon <= maxLon + M &&
        v.lat >= minLat - M && v.lat <= maxLat + M);
    }

    if (okFeeds === 0 || onMap.length === 0) {
      // All feeds down, or up-but-empty (off-hours). Simulate — badged.
      logSource('simulated', okFeeds === 0 ? 'all RT feeds unreachable' : 'RT feeds empty (off-hours)');
      const payload = simulated();
      cached = { payload, atMs: now() };
      return payload;
    }

    logSource('gtfs-rt', `${onMap.length} vehicles on-map (${vehicles.length} total, ${okFeeds}/${Object.keys(feeds).length} feeds)`);
    const payload: KumamotoVehiclesPayload = {
      updated: new Date(now()).toISOString(),
      source: 'gtfs-rt',
      vehicles: onMap,
    };
    cached = { payload, atMs: now() };
    return payload;
  }

  return {
    async get() {
      if (cached && now() - cached.atMs < RT_CACHE_MS) {
        // Re-derive simulated positions inside the cache window so cached
        // fallback frames still advance; live frames are served verbatim.
        return cached.payload.source === 'simulated' ? simulated() : cached.payload;
      }
      if (inflight) return inflight;
      inflight = sweep()
        .catch(() => simulated()) // belt-and-suspenders; sweep shouldn't throw
        .finally(() => { inflight = null; });
      return inflight;
    },
  };
}

/** Re-exported so the server can log route stats without re-reading the file. */
export function networkStats(net: KumamotoNetwork): string {
  const routes = net.operators.reduce((n, o) => n + o.routes.length, 0);
  const km = net.operators.reduce(
    (n, o) => n + o.routes.reduce((m, r) => m + r.lines.reduce((k, l) => k + lineLengthMeters(l), 0), 0), 0) / 1000;
  return `${net.operators.length} operators, ${routes} routes, ${Math.round(km)} km`;
}
