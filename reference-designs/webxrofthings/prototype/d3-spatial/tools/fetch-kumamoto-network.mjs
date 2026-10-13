#!/usr/bin/env node
/**
 * UC6 build-time snapshot — Kumamoto transit network geometry.
 *
 * Downloads routes.geojson + stops.geojson for the city tram and the three
 * city bus operators from the national GTFS repository (gtfs-data.jp),
 * simplifies the geometry, and writes one compact snapshot to
 * public/maps/kumamoto-network.json. The snapshot is COMMITTED — builds and
 * demos never need this script or the network; re-run it occasionally to
 * pick up route changes (feeds are re-cut every few months).
 *
 * Sources & licenses (attribution rendered in the UC6 scene legend):
 *   - 熊本市電 (tram):        kumamoto-shiden/kumamotoshiden — CC BY 2.1 JP
 *   - 熊本都市バス:            kumamoto-toshibus              — CC BY 4.0
 *   - 熊本バス:                kuma-bus                       — CC BY 4.0
 *   - 熊本電鉄バス:            kumamotodentetsu-bus           — CC BY 4.0
 *
 * Usage: node tools/fetch-kumamoto-network.mjs [--out <path>]
 */

import { writeFileSync, mkdirSync } from 'fs';
import { dirname, resolve } from 'path';
import { fileURLToPath } from 'url';

const here = dirname(fileURLToPath(import.meta.url));
const outArg = process.argv.indexOf('--out');
const OUT = outArg > -1
  ? resolve(process.argv[outArg + 1])
  : resolve(here, '..', 'public', 'maps', 'kumamoto-network.json');

const LISTING = 'https://api.gtfs-data.jp/v2/files?pref=43';

/** feed_id → { key, kind } we keep (decision #3: tram + 3 city operators). */
const KEEP = {
  kumamotoshiden: { key: 'tram', kind: 'tram', nameJa: '熊本市電' },
  'kumamoto-toshibus': { key: 'toshibus', kind: 'bus', nameJa: '熊本都市バス' },
  'kuma-bus': { key: 'kumabus', kind: 'bus', nameJa: '熊本バス' },
  'kumamotodentetsu-bus': { key: 'dentetsu', kind: 'bus', nameJa: '熊本電鉄バス' },
};

const round4 = (n) => Math.round(n * 1e4) / 1e4; // ≈ 10 m — plenty at tabletop scale

/** Decimate a coordinate ring: keep every point that moves ≥ ~25 m from the
 *  last kept one (plus endpoints). City-scale tabletop needs nothing finer. */
function simplify(coords) {
  if (coords.length <= 2) return coords.map(c => [round4(c[0]), round4(c[1])]);
  const MIN_DEG = 0.00025; // ≈ 25 m
  const out = [coords[0]];
  for (let i = 1; i < coords.length - 1; i++) {
    const last = out[out.length - 1];
    if (Math.abs(coords[i][0] - last[0]) + Math.abs(coords[i][1] - last[1]) >= MIN_DEG) {
      out.push(coords[i]);
    }
  }
  out.push(coords[coords.length - 1]);
  return out.map(c => [round4(c[0]), round4(c[1])]);
}

/** Clip a polyline to a bbox: emit the inside runs as separate lines.
 *  (Segment-level: a point outside the box ends the current run. No edge
 *  intersection math — at 25 m granularity the visual difference is nil.) */
function clipToBbox(coords, [minLon, minLat, maxLon, maxLat]) {
  const runs = [];
  let run = [];
  for (const c of coords) {
    const inside = c[0] >= minLon && c[0] <= maxLon && c[1] >= minLat && c[1] <= maxLat;
    if (inside) {
      run.push(c);
    } else if (run.length) {
      if (run.length >= 2) runs.push(run);
      run = [];
    }
  }
  if (run.length >= 2) runs.push(run);
  return runs;
}

async function getJson(url, attempts = 3) {
  for (let i = 1; ; i++) {
    try {
      const resp = await fetch(url, { headers: { 'accept-encoding': 'gzip' } });
      if (!resp.ok) throw new Error(`${url} → HTTP ${resp.status}`);
      return await resp.json();
    } catch (e) {
      if (i >= attempts) throw e;
      console.warn(`  retry ${i}/${attempts - 1} after: ${e.message ?? e}`);
      await new Promise(r => setTimeout(r, 1500 * i));
    }
  }
}

const listing = await getJson(LISTING);
const feeds = listing.body.filter(f => KEEP[f.feed_id]);
if (feeds.length !== Object.keys(KEEP).length) {
  const got = feeds.map(f => f.feed_id).join(', ');
  console.warn(`[kumamoto-network] warning: expected 4 feeds, matched: ${got || 'none'}`);
}

/* Two passes: the TRAM defines the city core. Its bbox (plus margin) clips
 * every bus operator's geometry — "spatial city map of Kumamoto", not a
 * prefecture-wide route atlas. Far-flung suburban branches fall away. */
const raw = [];
for (const feed of feeds) {
  const meta = KEEP[feed.feed_id];
  console.log(`[kumamoto-network] ${meta.key} (${feed.feed_id}) …`);
  const [routesGj, stopsGj] = await Promise.all([
    getJson(feed.file_route_url),
    getJson(feed.file_stop_url),
  ]);
  raw.push({ meta, feed, routesGj, stopsGj });
}

const tramRaw = raw.find(r => r.meta.kind === 'tram');
let tramBbox = null;
if (tramRaw) {
  let a = Infinity, b = Infinity, c = -Infinity, d = -Infinity;
  for (const f of tramRaw.routesGj.features ?? []) {
    const lines = f.geometry?.type === 'LineString' ? [f.geometry.coordinates]
      : f.geometry?.type === 'MultiLineString' ? f.geometry.coordinates : [];
    for (const l of lines) for (const [lon, lat] of l) {
      if (lon < a) a = lon; if (lon > c) c = lon;
      if (lat < b) b = lat; if (lat > d) d = lat;
    }
  }
  const MARGIN = 0.035; // ≈ 3.5 km around the tram network
  tramBbox = [a - MARGIN, b - MARGIN, c + MARGIN, d + MARGIN];
  console.log(`[kumamoto-network] city-core bbox (tram + margin): ${tramBbox.map(round4).join(', ')}`);
}

const operators = [];
for (const { meta, feed, routesGj, stopsGj } of raw) {
  // routes.geojson: FeatureCollection of LineString / MultiLineString per route.
  const routes = [];
  for (const f of routesGj.features ?? []) {
    const props = f.properties ?? {};
    let lines = f.geometry?.type === 'LineString' ? [f.geometry.coordinates]
      : f.geometry?.type === 'MultiLineString' ? f.geometry.coordinates
      : [];
    // Tram defines the core, so it is never clipped; buses are.
    if (tramBbox && meta.kind !== 'tram') {
      lines = lines.flatMap(l => clipToBbox(l, tramBbox));
    }
    const simplified = lines.map(simplify).filter(l => l.length >= 2);
    if (!simplified.length) continue;
    // gtfs-data.jp GeoJSON properties: { id, route_name: "O2-0：熊本駅→…" }
    // where "O2" is the line code and "-0" a variant number.
    const rawName = String(props.route_name ?? props.route_short_name ?? props.route_long_name ?? '');
    routes.push({
      id: String(props.id ?? props.route_id ?? routes.length),
      name: rawName.split('：')[0] || rawName,           // "O2-0"
      lineCode: (rawName.split('：')[0] || rawName).split('-')[0], // "O2"
      lines: simplified,
    });
  }

  /* GTFS "routes" are mostly VARIANTS of the same line (toshibus: 135
   * route_ids over 32 line codes) with heavily overlapping geometry. Keep
   * one per LINE CODE — the variant with the most geometry — which is what
   * a city map draws anyway and gives the vehicle simulation clean paths. */
  const byLine = new Map();
  for (const r of routes) {
    const key = r.lineCode || r.name || r.id;
    const size = r.lines.reduce((n, l) => n + l.length, 0);
    const prev = byLine.get(key);
    if (!prev) byLine.set(key, { route: r, size, ids: [r.id] });
    else {
      prev.ids.push(r.id);
      if (size > prev.size) { prev.route = r; prev.size = size; }
    }
  }
  const deduped = [...byLine.values()].map(({ route, ids }) => ({
    id: route.id, name: route.name, lines: route.lines,
    // ALL variant route_ids under this line — lets the GTFS-RT adapter map
    // a live vehicle's trip.routeId back to a display name.
    ids,
  }));
  routes.length = 0;
  routes.push(...deduped);

  // stops.geojson: FeatureCollection of Points (clipped to the core too).
  const stops = [];
  for (const f of stopsGj.features ?? []) {
    const c = f.geometry?.type === 'Point' ? f.geometry.coordinates : null;
    if (!c) continue;
    if (tramBbox && meta.kind !== 'tram' &&
        (c[0] < tramBbox[0] || c[0] > tramBbox[2] || c[1] < tramBbox[1] || c[1] > tramBbox[3])) continue;
    stops.push([round4(c[0]), round4(c[1])]);
  }

  operators.push({
    key: meta.key, kind: meta.kind, nameJa: meta.nameJa,
    license: feed.feed_license_id,
    routes, stops,
  });
  const pts = routes.reduce((n, r) => n + r.lines.reduce((m, l) => m + l.length, 0), 0);
  console.log(`  ${routes.length} routes (${pts} pts after clip+simplify), ${stops.length} stops`);
}

// Bounding box across all geometry — the renderer fits its projection to this.
let minLon = Infinity, minLat = Infinity, maxLon = -Infinity, maxLat = -Infinity;
for (const op of operators) {
  for (const r of op.routes) for (const l of r.lines) for (const [lon, lat] of l) {
    if (lon < minLon) minLon = lon; if (lon > maxLon) maxLon = lon;
    if (lat < minLat) minLat = lat; if (lat > maxLat) maxLat = lat;
  }
}

const snapshot = {
  generated: new Date().toISOString(),
  source: 'https://gtfs-data.jp (国土交通省 GTFSデータリポジトリ)',
  bbox: [round4(minLon), round4(minLat), round4(maxLon), round4(maxLat)],
  operators,
};

mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, JSON.stringify(snapshot));
const kb = Math.round(Buffer.byteLength(JSON.stringify(snapshot)) / 1024);
console.log(`[kumamoto-network] wrote ${OUT} (${kb} KB, bbox ${snapshot.bbox.join(', ')})`);
