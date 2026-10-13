# UC5 & UC6 design proposal — live geo dataspaces (Japan temperatures · Kumamoto transit)

Status: **Approved 2026-07-14** (decisions locked, see Part 5) · Captured: 2026-07-14 · Scope: `prototype/d3-spatial/`

Two new use cases that extend the prototype from *room/vehicle-scale* dataspaces to *geographic* ones:

- **UC5 (`DEMO05`) — Japan national temperature dataspace.** A spatial 3D map of Japan with an instantaneous-temperature scatterplot overlaid — the visual language of [Observable's global-temperature-trends](https://observablehq.com/@d3/global-temperature-trends) (a dot field colored on a diverging blue→red scale), but *instantaneous* instead of over-time: one live reading per prefecture, re-polled on a configurable interval.
- **UC6 (`DEMO06`) — Kumamoto city live-transit dataspace.** A spatial city map of Kumamoto with live bus positions (GTFS-RT) moving along the real route network, tram lines drawn from exact geometry.

Both were researched with live endpoint verification (2026-07-14). Simulated data ships first in both cases; the live adapters are drop-in swaps behind the same URLs.

---

## Part 1 — Data sources (verified)

### 1.1 UC5: JMA AMeDAS primary, Open-Meteo fallback

**Primary — JMA AMeDAS bosai JSON (recommended). No API key, official station observations, verified working.**

| | |
|---|---|
| Latest timestamp | `GET https://www.jma.go.jp/bosai/amedas/data/latest_time.txt` → `2026-07-14T01:00:00+09:00` |
| All-stations snapshot | `GET https://www.jma.go.jp/bosai/amedas/data/map/{yyyyMMddHHmmss}.json` (~250–350 KB, 1,286 stations, 916 with `temp`) |
| Station metadata | `GET https://www.jma.go.jp/bosai/amedas/const/amedastable.json` (lat/lon as `[deg, decimal-min]`, names) |

- Every value is `[value, qualityFlag]` — flag `0` = good, else treat as null. `temp` is °C.
- **10-minute observation cadence**, published with a 1–6 min lag → always resolve the timestamp from `latest_time.txt` rather than computing it (absorbs lag + clock skew).
- **One ~300 KB GET covers all 47 prefectures** — dramatically politer than 47 per-location calls to any commercial API.
- No prefecture field in the data; the **first 2 digits of the station ID are the JMA area code** (11–24 Hokkaido, 44 Tokyo, 86 Kumamoto, 91–94 Okinawa…). Simplest: hardcode the 47 verified capital-city station IDs (list captured in the research, e.g. Tokyo `44132`, Sapporo `14163`, Naha `91197`). Key by station ID, never by `enName` (duplicates exist).
- CORS-open (`access-control-allow-origin: *`), `cache-control: max-age=60`.
- Terms: unofficial-but-public (the JSON behind JMA's own site); JMA content is under Japan's Standard Terms of Use v2.0 (CC BY 4.0-compatible). Attribute **"Source: Japan Meteorological Agency (気象庁)"**. No SLA — keep a fallback, don't poll sub-minute.

**Fallback — Open-Meteo.** One request with 47 comma-separated lat/lons, no key: `https://api.open-meteo.com/v1/forecast?latitude=…&longitude=…&current=temperature_2m&timezone=Asia/Tokyo` → array of 47 `{current:{temperature_2m}}` objects (verified, 16 KB, values tracked AMeDAS well). Model analysis (15-min updates), not station readings. Free tier is non-commercial, **10k calls/day where a 47-location request counts as 47** → 10-min polling fits (~6.8k/day), 5-min does not. Attribute "Weather data by Open-Meteo.com" (CC BY 4.0).

Not chosen: OpenWeatherMap / WeatherAPI (key + 47 calls per poll, nothing distinctive), official JMA XML feeds (push-style JMX XML, heavy, no simple "current temp" pull).

### 1.2 UC6: Bus-Vision GTFS-RT (open!) — skip the CKAN portal

**The portal resource the request linked to is the wrong door.** Dataset `430005_gpts` on `datacatalogportal.dlp-kumamoto.jp` exposes a FIWARE Orion NGSI v2 API behind OAuth2 (verified `401` — WSO2 gateway wants `Authorization: Bearer` or `apikey`). Registration is **manual**: download an Excel 利用者登録申請書, email it to the prefecture's Digital Strategy Promotion Division (`dejisuishin@pref.kumamoto.lg.jp`), wait for credentials by reply. No self-serve, no SLA.

**The same data is fully open, no auth, at the source** — the joint bus operators' Bus-Vision open-data page (`km.bus-vision.jp/kumamoto/view/opendataKuma.html`), CC BY 4.0, all URLs verified live:

- **GTFS-RT protobuf**, pattern `https://km.bus-vision.jp/realtime/{op}_{kind}_update.bin` — `{op}` ∈ `sankobus | dentetsu | kumabus | toshibus`, `{kind}` ∈ `vpos` (VehiclePosition) / `trip` (TripUpdate) / `alrt` (Alert). Valid `FeedMessage`, gtfs-realtime v2.0. **Updated every 15 s; the page explicitly asks not to poll faster.** Empty (header-only) outside service hours (~06:00–23:00 JST).
- **Static GTFS-JP zips** per operator at `https://km.bus-vision.jp/gtfs/{op}/gtfsFeed`. Caveat: **no `shapes.txt`** in the bus feeds — route geometry comes from ordered stop sequences, or better, prebuilt GeoJSON below.
- **Tram (熊本市電): static GTFS only** — on the national GTFS repository ([gtfs-data.jp](https://gtfs-data.jp)), **with `shapes.txt`** (exact line geometry) plus prebuilt `stops.geojson` / `routes.geojson`. CC BY 2.1 JP (熊本市交通局). **No public tram GTFS-RT** — tram renders as a static layer; live vehicles are bus-only.
- **Mirror/fallback:** `https://api.gtfs-data.jp/v2/files?pref=43` lists all six Kumamoto feeds with stable `feed.zip` + `stops.geojson` + `routes.geojson` URLs (bus records embed the same RT URLs, `update_interval: 15`). No ODPT mirror exists.

So: **no API key, no registration, for everything UC6 needs.** If prefecture-platform access is wanted later for other datasets, the Excel-application route exists, but it gates nothing here.

---

## Part 2 — Visualization design

### 2.1 The shared substrate: a `geo-scene` mark family

Capability audit findings (from the current codebase):

- **No geo capability exists today**: no `d3-geo`, no map/GeoJSON/terrain mark, no lat/lon→position code. The only "map" is the three-globe inside the `imu` cell.
- The closest primitive is the **`targets`** cell (`demo/liveVitalsCells.ts`) — a self-polling mark that places data-driven glyphs at `x_m/y_m` on a 2D plane (capped at 3 glyphs).
- Texture-on-plane, InstancedMesh glyphs, self-positioned marks (`config.position`, bypassing the grid), and self-fetching shapes (`video/imu/targets` skip loader prefetch) are all established patterns to build on.

Proposal: **one new mark type, `geo-scene`, serving both UCs** — a self-positioned, self-fetching cell composed of config-driven layers:

```
geo-scene
├─ basemap layer      (one of:)
│   ├─ 'tile-grid'         — pixel-map: one square tile per region, grid-arranged (UC5)
│   └─ 'geojson-lines'     — route polylines + stop dots on a dark ground plane (UC6)
├─ points layer       — live glyphs anchored to tiles (UC5) or projected lat/lon (UC6)
│   ├─ UC5: instanced columns rising from tiles, color+height = temperature
│   └─ UC6: instanced oriented arrows, color = route, positions tweened between polls
└─ legend/attribution — color ramp + source line (three-mesh-ui panel)
```

- **UC5 basemap is a tile-grid "pixel Japan"** (decision #1): the classic one-prefecture-one-square map, arranged in the standard Japan grid layout (Hokkaido top-right, Tōhoku column, Kantō block, … Okinawa offset bottom-left). The layout is a **47-row static lookup table** (`{prefCode, gridX, gridY}`) — no TopoJSON, no polygon extrusion, no geo projection needed for UC5 at all. Config `tileScale: 'uniform' | 'area'` — `area` scales each tile's footprint by prefecture land area (Demers-cartogram style, tiles stay anchored to their grid cell); default `uniform` ships first.
- **Projection (UC6 only):** add `d3-geo`. One `geoMercator().fitExtent()` computed once from the network bounds, shared by every layer — lat/lon in, plane-local x/y out. Scene laid flat on a tabletop-style tilted plane (~30–40°, per XR legibility lessons from UC3/UC4).
- **Basemaps are vector/procedural, not raster tiles** — deliberately. Tile-grid squares (UC5) and "the route network *is* the map" (UC6) avoid OSM/GSI tile licensing entirely and look better in XR than a flat photo texture. Kumamoto route GeoJSON comes from gtfs-data.jp at build time.
- **Why one mark type, not two:** both UCs are "geo basemap + live points." Config selects layers, exactly like `splat-gallery`/`video` are config-driven. Keeps `MARK_TYPES` growth linear and the builder tested once.

### 2.2 UC5 look — instantaneous-temperature pixel map of Japan

- 47 tiles (one per prefecture, capital-city station reading — decision #2), each with a **column rising from the tile**; height and color both encode °C (redundant encoding reads better in XR than color alone). The tile top face tints with the same scale, so even at glancing angles the map reads as a heat-mosaic.
- **Color scale:** diverging **blue → white → red** (`d3.interpolateRdBu` reversed), the palette of the referenced Observable piece. Domain anchored to a fixed seasonal window (e.g. −5…35 °C summer config) so colors are comparable across polls, with the current **national mean as the white midpoint** shown in the legend. Hovering a glyph shows `prefecture · XX.X °C · HH:MM JST` (existing hover pattern).
- On each poll, glyphs **tween height/color over ~1 s** — the map visibly "breathes" every refresh without popping.
- Optional second HUD view (`show-only:` wiring, like UC4's mode switch): a ranked bar strip of hottest→coldest prefectures, reusing the existing `bar` mark against the same endpoint.

### 2.3 UC6 look — live vehicles on the Kumamoto network

- Dark ground plane, **route polylines color-coded by operator/route**, stops as faint dots, tram lines (exact `shapes.txt` geometry) visually distinct (brighter/wider).
- Live buses = **instanced arrow/wedge glyphs** oriented by GTFS-RT `bearing`, colored by route. Positions **interpolate between 15 s polls** (linear tween along heading) so motion is continuous rather than teleporting; a staleness indicator dims vehicles whose `timestamp` is old.
- Off-hours (feeds empty overnight JST): the cell automatically falls back to **simulation mode** — synthetic vehicles advancing along the real `routes.geojson` polylines — so the demo is *never* blank. Badged "SIMULATED" exactly like the existing DEMO-MODE badge.
- Scale: city-scale scene (~1.2 m wide tabletop), `config.position` places it; grab-to-rotate reuses the `force-tree-3d` interaction.

---

## Part 3 — Data plumbing

Follows the established two-tier pattern (mock-join-server owns simulated/adapter endpoints; Vite proxy routes):

### 3.1 New server endpoints (`server/mock-join-server.ts`)

| Endpoint | Returns | Mode |
|---|---|---|
| `GET /api/v1/geo/japan-temps` | `{ updated, source, stations:[{id, name, prefCode, lat, lon, tempC, obsTime}] }` | **live-adapter**: fetch `latest_time.txt` → `map/{ts}.json` (cache by timestamp — free dedupe since JMA only changes every 10 min) → pluck 47 station IDs, null out flag≠0. Falls back per-station to same-prefix stations, then to **deterministic simulation** (sinusoid over a July baseline table) when offline. `source` field says `jma | open-meteo | simulated`. |
| `GET /api/v1/geo/kumamoto/vehicles` | `{ updated, source, vehicles:[{id, routeId, routeName, lat, lon, bearing, ts}] }` | **live-adapter**: poll the 3 city-operator `vpos` bins (server-side timer, ≥15 s, **one upstream fetch per interval regardless of client count** — same serialize-at-the-proxy discipline as the ESP `maxSockets:1` rule), decode with `gtfs-realtime-bindings`, merge. Empty feed or fetch failure → **simulation**: synthetic vehicles walking the bundled route polylines. |
| `GET /api/v1/geo/kumamoto/network` | routes + stops GeoJSON (static, bundled) | served from a build-time snapshot in `public/maps/` |

Deterministic simulated modes ship **first** (Phase 1/3) and remain the permanent fallback — this bakes in the graceful-degradation lesson from the blank-screen fix: the endpoint always answers, the cell always renders, badges tell the truth.

**Proxy:** `/api/v1/geo/*` has no collision with any real-device prefix, so the existing `/api/v1` catch-all → `localhost:3001` routes it with **zero vite.config.ts changes**. (Deliberately *not* under `/api/v1/sensor/`, which would collide with the Atom-Echo rule and force a carve-out.)

### 3.2 Manifest & join wiring

- `examples/uc5-japan-temps.json`, `examples/uc6-kumamoto-transit.json` — each: one `geo-scene` mark (self-positioned, `defaultVisible: true`), UDM/USM entries describing the upstream feeds as virtual devices, HUD with recenter/leave (+ UC5's ranked-bars `show-only` toggle).
- Data source on the points layer: `{ source:'url', url:'/api/v1/geo/…', shape:'geo-points', refreshInterval: 300 }` (UC5; JMA changes every 600 s, so 300 s halves worst-case staleness at trivial cost — floor of 60) and `refreshInterval: 15` (UC6, matching upstream exactly).
- `geo-points` joins `SELF_FETCHING_SHAPES` (cell polls itself, like `targets`) → no loader/fakeData plumbing needed.
- Registration: `MARK_TYPES` + `URL_DATA_SHAPES` in `schema.ts`, builder in `builders.ts`, `SELF_POSITIONED` in `renderManifest.ts`, **`manifest.schema.json` sync** (it's already stale — 11 types missing; fix while touching it), `DEMO05`/`DEMO06` in `DEFAULT_FIXED_CODES` (mock-join-server) **and** in `DEMO_MANIFESTS` (main.ts offline fallback map from the blank-screen fix).

### 3.3 Polling & rate-limit policy (explicit, since the user asked)

| Feed | Upstream cadence | Our default | Hard floor | Why safe |
|---|---|---|---|---|
| JMA AMeDAS | 10 min | **5 min** | 60 s | one ~300 KB GET per poll, CDN-served, cache-by-timestamp dedupes to ≤6 real fetches/hour |
| Open-Meteo (fallback only) | 15 min | 10 min | 10 min | 47-loc request = 47 calls; 10-min = ~6.8k/day < 10k/day free cap |
| Bus-Vision GTFS-RT | 15 s | **15 s** | 15 s | matches upstream exactly; page explicitly forbids faster; server-side single-flight shields upstream from client fan-out |

All intervals are manifest-configurable via `refreshInterval`; the server clamps to the floors so a mis-authored manifest can't hammer anyone.

### 3.4 Attribution (rendered in each scene's legend panel)

- UC5: 「出典: 気象庁」/ "Source: Japan Meteorological Agency" (+ "Weather data by Open-Meteo.com" when the fallback is active — the `source` field drives this live).
- UC6: "Bus data CC BY 4.0 — 九州産交バス・熊本電鉄・熊本バス・熊本都市バス (km.bus-vision.jp)" + "Tram data CC BY 2.1 JP — 熊本市交通局 (gtfs-data.jp)".

---

## Part 4 — Phased build plan

| Phase | Deliverable | Proves |
|---|---|---|
| **1** ✅ 2026-07-14 | `geo-scene` mark (tile-grid pixel-Japan basemap, temperature-column points layer) + `/api/v1/geo/japan-temps` **simulated** + `uc5-japan-temps.json` + `DEMO05` | the whole geo substrate, end-to-end, offline. Verified headless on both entry paths (`?manifest=` + DEMO05 join); 273 tests green. Notes: color ramp stretches over the *current spread* around the mean (fixed-domain mapping left the map all-neutral); cells self-drive via `onBeforeRender` (manifest pipeline has no per-frame tick) and follow the `setActive` show-only protocol. |
| **2** ✅ 2026-07-14 | JMA live adapter (+ Open-Meteo fallback) behind the same URL | live path is a server-side swap; zero client changes — confirmed: real 47/47-station JMA payload rendered with 出典: 気象庁 legend, no client edits. Adapter (`server/japanTempsAdapter.ts`): cache-by-JMA-timestamp (≤6 map fetches/hr), 60 s recheck floor, single-flight, Open-Meteo on a 10-min budget floor, simulation as final fallback. Live upstream only in the standalone entrypoint (`GEO_LIVE=0` to disable); `createJoinServer` stays hermetic for tests. |
| **3** ✅ 2026-07-15 | Kumamoto network layer (GeoJSON snapshot script) + **simulated** vehicles + `uc6-kumamoto-transit.json` + `DEMO06` | city-scale scene + motion tweening — verified headless on both entry paths: 4 operator line layers + stops render from the committed snapshot, 24 simulated buses walk real route polylines and tween between 15 s polls. Notes: snapshot (`tools/fetch-kumamoto-network.mjs` → `public/maps/kumamoto-network.json`, 945 KB) clips buses to the tram-defined city core and dedupes GTFS route *variants* to line codes (296→77 routes); network is served as a static asset (`/maps/…`), not an API route — simpler than §3.1's original table and identical at runtime; projection is hand-rolled equirect×cos(midLat) (city-scale — d3-geo unnecessary); operator palette validated (worst pair ΔE 15.7, tram amber intentionally above the band as the emphasized rail layer). |
| **4** ✅ 2026-07-15 | GTFS-RT live adapter (protobuf decode, single-flight cache, off-hours auto-sim) | live vehicles during JST service hours. Adapter (`server/kumamotoVehiclesAdapter.ts`, `gtfs-realtime-bindings`): 3 vpos feeds fetched in parallel per sweep, 15 s cache + single-flight (one upstream sweep per window regardless of clients — the publisher's stated cadence), bbox-filter to the rendered core, bearing derived from previous fix when the feed omits it, auto-fallback to simulation when feeds are empty/unreachable (`source` flips the scene badge). Live-verified at 03:57 JST: real sweep → "RT feeds empty (off-hours)" → simulated fallback served; entity decode covered by synthetic-protobuf tests (note: GTFS-RT positions are float32). **Remaining check: eyeball real vehicles during JST service hours (~06:00–23:00).** |
| **5** ◐ in progress | Polish: legend panels, staleness dimming, UC5 ranked-bars HUD view, on-device Quest pass | XR legibility. Landed 2026-07-16 (user feedback round 1): canvas-sprite kanji labels above every prefecture (`labelSprite.ts` — browser system fonts render CJK, no bundled font; billboard sprites) with per-station temp + ▲/▼ trend arrows (armed on ≥0.1 °C change vs previous reading, 20 min TTL); UC5 column heights now normalized to the current national min/max (hottest = tallest — the fixed domain compressed same-moment readings); map lowered/flattened for seated use (y 0.72, tilt 55°); UC6 vehicles carry hovering `line · vehicle-id` labels — snapshot now maps ALL GTFS variant route_ids to line codes so live GTFS-RT routeIds resolve (verified live: real bus kumabus-141 → line M2-1). Also fully verified live GTFS-RT vehicles end-to-end (badge "LIVE · N vehicles"). Remaining: ranked-bars HUD view, per-glyph hover interactions, on-device Quest/Spectacles pass. |

Each phase lands green: `schema.test.ts` fixture rows for both manifests (note: UC1/3/4 currently aren't fixture-validated — add all), builder tests for `geo-scene` (projection math, glyph counts, tween state), supertest rows for the three routes (simulated determinism + live-adapter mocked upstream), and a Playwright smoke screenshot per UC (matching the existing ~99-screenshot discipline).

## Part 5 — Decisions (locked 2026-07-14)

1. **UC5 map style: tile-grid "pixel Japan".** One square per prefecture in the standard grid arrangement, with an optional area-scaled tile footprint (`tileScale: 'area'`). Replaces the originally-proposed extruded polygons — simpler (a 47-row lookup table instead of TopoJSON + extrusion) and matches the familiar pixel-map idiom.
2. **UC5 glyph density: 47 prefectures** (capital-city stations), one reading each.
3. **UC6 operator scope: 3 city operators** (toshibus, kumabus, dentetsu); prefecture-wide `sankobus` can be added later behind config.
4. **Tram: static lines only** (no tram realtime feed exists; honest rendering).

---

## References

- Research verified 2026-07-14: JMA endpoints fetched live (payload excerpts above); Bus-Vision feeds fetched live (valid `FeedMessage`); CKAN portal auth tested (`401`, manual Excel-application flow documented).
- Capability audit: `src/manifest/schema.ts` (MARK_TYPES, URL_DATA_SHAPES), `src/manifest/loader.ts` (SELF_FETCHING_SHAPES), `demo/liveVitalsCells.ts` (`targets` pattern), `renderManifest.ts` (SELF_POSITIONED), `server/mock-join-server.ts` (simulated-feed pattern), `vite.config.ts` (proxy tiers).
- Visual reference: [Observable — Global temperature trends](https://observablehq.com/@d3/global-temperature-trends) (diverging RdBu dot field).
- Data: [JMA AMeDAS bosai JSON](https://www.jma.go.jp/bosai/amedas/data/map/), [Open-Meteo](https://open-meteo.com), [Bus-Vision Kumamoto open data](https://km.bus-vision.jp/kumamoto/view/opendataKuma.html), [GTFSデータリポジトリ](https://gtfs-data.jp), [dlp-kumamoto portal](https://datacatalogportal.dlp-kumamoto.jp/odp/about/).
