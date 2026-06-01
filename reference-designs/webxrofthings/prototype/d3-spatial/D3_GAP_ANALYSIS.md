# D3 → d3-spatial: Gap Analysis & Spatialisation Roadmap

A practitioner's audit of what d3-spatial currently covers from the full D3.js v7 surface, what it doesn't, and — for each gap — whether and how it should be spatialised. The goal is to grow d3-spatial from a "23-mark prototype" into a comprehensive D3-class spatial visualisation toolkit.

State as of M22 (2026-05-24).

---

## 1. D3.js modules — coverage map

D3 is a federation of ~30 small modules. Many are utilities (`d3-array`, `d3-color`, `d3-interpolate`) that we already embed implicitly. Others (`d3-selection`, `d3-zoom`, `d3-drag`) are DOM-bound and don't translate. The interesting column is **layout / visual / interaction primitives that d3-spatial *should* surface but doesn't yet.**

| D3 module | Status in d3-spatial | Notes |
|---|---|---|
| `d3-array` | implicit | extent/min/max/quantile used inside builders |
| `d3-axis` | hand-rolled | `src/chart/Chart.ts` builds tick lines in three.js, doesn't share axis logic across marks |
| `d3-brush` | ❌ MISSING | range selection — see §3, item B1 |
| `d3-chord` | ❌ MISSING | chord diagram — see §2, item N3 |
| `d3-color` | implicit | three.js Color subsumes most needs |
| `d3-contour` | ❌ MISSING | contour / density — see §2, item S5 |
| `d3-delaunay` | partial | used by `voronoi-stippling`; not exposed as a general layout |
| `d3-dispatch` | N/A | three.js EventDispatcher fills the role |
| `d3-drag` | replaced | `Interact.ts` + `DragBrush.ts` + `FingertipGrab.ts` |
| `d3-dsv` | ❌ MISSING | no CSV/TSV ingest from manifest — see §4 |
| `d3-ease` | partial | `util/easings.ts` covers the common ones |
| `d3-fetch` | implicit | loader.ts uses fetch directly |
| `d3-force` | replaced by `d3-force-3d` | force + force-tree-3d builders |
| `d3-format` | implicit | used in label formatters |
| `d3-geo` | ❌ MISSING | projections / topojson — see §2, item G1–G4 |
| `d3-hierarchy` | ✅ covered | tree, treemap, sunburst, pack, tidy-tree, tangled-tree, force-tree-3d |
| `d3-interpolate` | implicit | three.js Color/Vector lerp |
| `d3-path` | N/A | three.js geometry replaces SVG paths |
| `d3-polygon` | partial | hull/centroid used inside voronoi-stippling |
| `d3-quadtree` | implicit | force-3d uses it internally |
| `d3-random` | implicit | Math.random; deterministic seeding in `fakeData.ts` |
| `d3-scale` | implicit | each builder makes its own scale; no shared `Scale` primitive — see §3, item L1 |
| `d3-scale-chromatic` | partial | a handful of warm palettes; no diverging/sequential schemes — see §3, item L2 |
| `d3-selection` | N/A | DOM-bound, no equivalent needed in three.js scene-graph |
| `d3-shape` | partial | line + arc + bar built directly; missing pie, area, symbol, custom curves — see §2, item C1–C5 |
| `d3-time` | implicit | Date arithmetic in `fakeData.ts` and chart x-axes |
| `d3-time-format` | implicit | label formatters |
| `d3-timer` | N/A | three.js render loop is the timer |
| `d3-transition` | replaced | `util/tween.ts` + `tweenInstanced` |
| `d3-zoom` | physical | walking up to a mark *is* the zoom; no DOM zoom needed |

**Bottom line:** of the 30 D3 modules, ~10 are implicit-by-existence (utilities), ~5 don't translate to XR, ~5 are functionally covered by three.js or our own systems, and **~10 represent real spatialisation opportunities** — most notably `d3-chord`, `d3-contour`, `d3-geo`, more of `d3-shape`, `d3-brush`, and a unified `d3-scale`/legend story.

---

## 2. Mark-type gaps (the D3 gallery, spatialised)

A walk through the canonical D3 gallery taxonomy. Each row is rated:

- **Win** — strong spatial argument; the third dimension carries real meaning, not decoration.
- **Fit** — works in XR; spatialisation is incremental, not transformative.
- **Skip** — better in 2D; spatialising adds nothing or hurts.

Priority is a coarse ordering for a "if you build one this quarter, build this" roadmap.

### Hierarchy (mostly done)

| | Type | Status | Priority | Spatial encoding |
|---|---|---|---|---|
| H1 | Tree (radial / tidy / cluster) | ✅ tidy-tree, tangled-tree, force-tree-3d | — | depth = radial / column |
| H2 | Treemap | ✅ | — | extruded rectangles, value = height |
| H3 | Sunburst | ✅ | — | depth = ring index |
| H4 | Circle pack | ✅ | — | nested spheres |
| H5 | **Icicle / partition** | ❌ MISSING | Win, medium | flat-sunburst-as-tower; depth = vertical levels, value = block height. Fly *through* the hierarchy. |
| H6 | **Dendrogram (cluster)** | ❌ MISSING | Fit, low | flat-leaf variant of tidy tree; one-day work to add `align: 'leaves'` to tree builder. |

### Networks / graphs (half done)

| | Type | Status | Priority | Spatial encoding |
|---|---|---|---|---|
| N1 | Force-directed graph | ✅ | — | x/y/z node positions |
| N2 | Sankey | ✅ | — | extruded ribbons |
| N3 | **Chord diagram** | ❌ MISSING | Win, high | wrap a torus around the user; ribbons thread through the donut hole. Strong scale-up to many categories in XR vs. the SVG version. |
| N4 | **Arc diagram** | ❌ MISSING | Fit, low | nodes on a baseline + curved connections; the 3rd dimension can encode time or weight |
| N5 | **Adjacency matrix** | ❌ MISSING | Win, high | tilted grid; **fly through** to compare structure with the same data drawn as N1 force-graph side-by-side. Doubles as a brushing-across-views demo for L4. |
| N6 | **Hive plot** | ❌ MISSING | Fit, medium | three radial axes meeting at origin; connections curve through them. Natural cylindrical layout in XR. |
| N7 | Edge bundling | ✅ (`edgeBundle` viz, not yet registered as a builder) | medium | already built, surface in manifest |
| N8 | **Bipartite / multipartite** | ❌ MISSING | Fit, low | layered force-graph with column constraints |

### Statistical / distribution (gaps here)

| | Type | Status | Priority | Spatial encoding |
|---|---|---|---|---|
| S1 | Streamgraph | ✅ | — | thickness × time |
| S2 | Ridgeline | ✅ | — | row-per-distribution |
| S3 | **Histogram** | ❌ MISSING | Win, high | trivial — extruded bar mark with binning. Foundational; many demos want this. |
| S4 | **Hexbin density** | ❌ MISSING | Win, high | 3D extrusion is *the* canonical spatial use of hex binning. Plug in `d3-hexbin`. |
| S5 | **Contour / density** | ❌ MISSING | Win, high | `d3-contour` outputs marching-square iso-bands → extrude as 3D terrain. Reads beautifully through passthrough. |
| S6 | **Box plot** | ❌ MISSING | Win, medium | box = 3D box (IQR), whiskers extruded on Z, outliers as floating spheres. Side-by-side comparison of categories in row. |
| S7 | **Violin plot** | ❌ MISSING | Fit, medium | per-category ridge, mirrored. Effectively ridgeline-per-category. |
| S8 | **Beeswarm** | ❌ MISSING | Fit, medium | nodes packed without overlap along a categorical axis. Use the force layout in 1D with collision. |
| S9 | **Strip plot / jitter** | ❌ MISSING | Skip | beeswarm subsumes it |
| S10 | **Q-Q plot** | ❌ MISSING | Skip | niche; 2D works fine, XR adds nothing |

### Geographic (entire pillar missing)

| | Type | Status | Priority | Spatial encoding |
|---|---|---|---|---|
| G1 | Globe (3D sphere) | partial (`globeWidget`) | high | already a globe; needs data binding |
| G2 | **Choropleth map** | ❌ MISSING | Win, high | `d3-geo` + topojson; render countries/regions as colored extruded prisms on the globe. Height = value. |
| G3 | **Flow / arc map** | ❌ MISSING | Win, high | great-circle arcs between lat/lon pairs. The killer XR geographic viz — feels like a real Earth-data console. |
| G4 | **Cartogram** | ❌ MISSING | Fit, low | requires `d3-cartogram` external; non-trivial layout |
| G5 | **Topojson terrain** | ❌ MISSING | Fit, medium | elevation field as 3D heightfield; combines with G2 for true-3D country shapes |
| G6 | **Bubble map** | ❌ MISSING | Fit, medium | point-data + size encoding on globe; near-trivial atop G1 |

### Categorical / comparison

| | Type | Status | Priority | Spatial encoding |
|---|---|---|---|---|
| C1 | Bar | ✅ | — | value = height |
| C2 | **Stacked bar** | ❌ MISSING | Fit, high | trivial — extend bar mark with `stack: true`. Common ask. |
| C3 | **Grouped bar** | ❌ MISSING | Fit, high | side-by-side along Z-axis (depth dimension free in XR!) |
| C4 | **Pie / donut** | ❌ MISSING | Fit, medium | extrude wedges → "wedge cake"; height = secondary value (double-encoding pays for 3D) |
| C5 | **Radial bar / polar area** | ❌ MISSING | Fit, medium | wedges of constant angle, variable radius/height. Nightingale rose in three dimensions. |
| C6 | **Radar / spider chart** | ❌ MISSING | Win, medium | natural as a 3D polyhedron when comparing 5+ axes; multi-entity comparison via overlapping translucent hulls. |
| C7 | **Marimekko (mosaic)** | ❌ MISSING | Skip | 2D works; spatialising hurts readability |
| C8 | **Waffle / unit chart** | ❌ MISSING | Skip | text-dense; XR doesn't help |
| C9 | **Slope chart** | ❌ MISSING | Skip | line subsumes the use case |
| C10 | **Bump chart** | ❌ MISSING | Win, low | rank-vs-time; spatialise as ribbons over time — could be very evocative |

### Time

| | Type | Status | Priority | Spatial encoding |
|---|---|---|---|---|
| T1 | Line | ✅ | — | x = time |
| T2 | **Calendar heatmap** | ❌ MISSING | Win, high | year × week × day-of-week → 3D brick field; value = brick height + color. Reads instantly. |
| T3 | **Horizon chart** | ❌ MISSING | Skip | designed to compress vertical space in 2D; XR has unlimited vertical |
| T4 | **Bar-chart race / animated** | ❌ MISSING | Fit, low | time scrubber + tweened bars; works with existing bar mark + tween |
| T5 | **Cycle plot** | ❌ MISSING | Skip | niche |

### Multivariate

| | Type | Status | Priority | Spatial encoding |
|---|---|---|---|---|
| M1 | Parallel coordinates | ✅ (`parallel.ts`, viz not yet a registered builder) | medium | surface in manifest |
| M2 | Scatter / bubble | ✅ scatter | — | x/y/z natively |
| M3 | **Scatter-3D + size + color** | ✅ scatter (Win, low) | — | already 3D; document examples |
| M4 | **Splom (scatter-matrix)** | ❌ MISSING | Fit, low | grid of scatters — useful in XR because you can walk along it instead of squinting |
| M5 | **Linked highlight across views** | ❌ MISSING | Win, medium | see §3 L4 |

### Specialty / "wow"

| | Type | Status | Priority | Spatial encoding |
|---|---|---|---|---|
| W1 | Voronoi stippling | ✅ | — | UC3 |
| W2 | Moon phases arc | ✅ | — | UC3 |
| W3 | Owls grid (raster) | ✅ | — | UC3 |
| W4 | **Word cloud** | ❌ MISSING | Skip | dense 2D layout; XR makes text harder, not easier |
| W5 | **3D point cloud / splat** | ✅ splat-gallery | — | UC4 |
| W6 | **Sun-shadow / orrery** | ❌ MISSING | Fit, low | natural in XR but niche |

---

## 3. Behaviors & interactions

D3's `selection.on()`, `d3-brush`, `d3-zoom`, and `d3-transition` give vanilla D3 a coherent interaction layer. d3-spatial has hover (NodeHoverFx), grab (FingertipGrab), drag (DragBrush), and tweens — but lacks several systems whose absence is felt:

| | Capability | Status | Notes |
|---|---|---|---|
| B1 | **3D brush (range select)** | ❌ MISSING | drag a hand-defined box through a scatter/contour to select; emits selection events. Foundational for B2. |
| B2 | **Linked highlight (brushing across views)** | ❌ MISSING | hover/select on mark A → corresponding data point lights up on every other mark in the dataspace. The single biggest "XR earns its keep" interaction. |
| B3 | **Crosshair / focus cursor** | ❌ MISSING | hand-anchored ruler on continuous charts; reads exact value where the beam intersects |
| B4 | **Lasso selection** | ❌ MISSING | scrubble a hand through space, points inside the swept volume get selected. Builds on FingertipGrab. |
| B5 | **Focus + context (head-pose driven LOD)** | ❌ MISSING | when the user is within 0.5 m of a mark, increase its visual complexity; dim distant marks. Trades render budget for engagement. |
| B6 | **Animated transitions between marks** | partial | tween + tweenInstanced exist; no "morph from bar → pie" demo wired up |
| B7 | **Tooltip on hover** | ✅ NodeHoverFx | strong; close to D3.tip parity for our use cases |
| B8 | **Inspector card** | ✅ InspectorCard | UC1/UC2 use this |

### Library primitives (no UI, but missing infra)

| | Capability | Status | Notes |
|---|---|---|---|
| L1 | **Shared `Scale` primitive** | ❌ MISSING | each builder makes its own scale; no `linearScale(domain, range)` / `logScale` / `quantizeScale` we can share. Refactor target. |
| L2 | **Color schemes (sequential + diverging)** | partial | only the warm palette + a handful of categorical hues; no `interpolateViridis`-equivalent baked in |
| L3 | **In-scene color & size legends** | ❌ MISSING | every chart that uses color or size for an encoding should advertise its scale. A floating strip with min/max labels is a 30-line component. |
| L4 | **Cross-mark event bus** | ❌ MISSING | needed for B2. Marks register interest in `(dataspace, dataPointId)`; an event router fans hover/select events out. |
| L5 | **`d3-shape` curve primitive library** | partial | `Catmull` & straight only; no `monotoneX`, `step`, `basis`, etc. |
| L6 | **`d3-format` exposed** | ❌ MISSING | every chart label formatter is ad hoc; expose `d3.format(',.2f')` style API |

---

## 4. Data ingest gaps

The manifest schema accepts `inline` and `url` data sources. Real D3 work routinely starts from CSV/TSV files or geojson. d3-spatial currently has no path.

| | Capability | Status | Notes |
|---|---|---|---|
| D1 | **CSV / TSV ingest** | ❌ MISSING | `{ source: 'url', shape: 'csv', schema: {...} }` — wraps `d3-dsv` |
| D2 | **GeoJSON / TopoJSON ingest** | ❌ MISSING | required for G2–G5 |
| D3 | **Streaming / WebSocket** | ❌ MISSING | polling works fine for our 5 Hz IMU + 1 Hz vitals; not pressing |
| D4 | **MQTT bridge** | ❌ MISSING | UC2 already publishes MQTT from devices; could ingest directly via mqtt-over-WS |

---

## 5. Proposed roadmap

A pragmatic "if you have N weeks, build this" ordering, accounting for both reach (how many demos benefit) and difficulty.

### Tier 1 — high-leverage, days each
1. **Stacked bar + grouped bar** (C2 + C3) — extend the existing bar mark with `stack` / `group` options.
2. **Histogram** (S3) — derivation atop bar with binning helper.
3. **Calendar heatmap** (T2) — extruded brick grid; reuse instanced cube mesh.
4. **In-scene color + size legends** (L3) — companion of every encoding-using mark.
5. **Shared `Scale` primitive** (L1) — refactor; removes duplication across all current builders.
6. **Surface `parallel` and `edgeBundle` as manifest builders** — already built in `src/viz/`, never registered.

### Tier 2 — high-leverage, ~1 week each
7. **Choropleth + flow-arc maps on the globe** (G2 + G3) — biggest single "wow" gap. Needs `d3-geo` + topojson. UC5 candidate: real-time weather/aviation dataspace.
8. **Hexbin density + contour heightfield** (S4 + S5) — natural XR encodings; one demo each.
9. **Box plot** (S6) — per-category comparison; useful for any aggregate dashboard.
10. **Chord diagram** (N3) — UC6 candidate: relationships dataspace (org chart, migration, trade flows).
11. **Adjacency matrix + the matrix↔node-link toggle demo** (N5) — pedagogically valuable; pairs with B2.

### Tier 3 — interaction layer
12. **3D brush** (B1).
13. **Cross-mark event bus** (L4) → **linked highlight** (B2). Pre-req for any "small multiples" dataspace.
14. **Focus+context LOD** (B5) — start with simple distance-based opacity.

### Tier 4 — completionism
15. Pie/donut (C4), radial bar (C5), radar (C6), beeswarm (S8), violin (S7), icicle (H5).
16. CSV/TSV ingest (D1) and `d3-format` surfacing (L6).
17. Hive plot (N6), bump chart (C10), splom (M4).

### Probably-never (3D adds nothing or actively hurts)
- Word cloud (W4)
- Horizon chart (T3)
- Marimekko / waffle (C7 / C8)
- Q-Q plot (S10)
- Cartogram (G4) — deferred unless a use case demands it

---

## 6. What we'd unlock

Filling Tiers 1–3 would let d3-spatial honestly claim:
- **All canonical D3 chart types** — bar/line/pie/scatter/area + their stacked, grouped, and radial variants.
- **All canonical hierarchy and network layouts** — tree, treemap, pack, sunburst, partition, chord, adjacency-matrix, force.
- **A real geographic story** — globe + choropleth + flow-arc maps. Currently the single biggest "feels unfinished" gap.
- **Linked-views interactivity** — the brushing-across-views demo is the strongest argument for *why XR* in data viz.
- **A complete scale + legend story** — every chart honestly advertises its encoding.

That's the difference between "a WebXR prototype with 23 hand-rolled visualisations" and "a credible spatial alternative to vanilla D3 for IoT and small-data dashboards." Tier 1 alone (≈two weeks of focused work) closes most of the credibility gap.
