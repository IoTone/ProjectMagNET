import * as THREE from 'three';
import { stack, stackOffsetWiggle, stackOrderInsideOut } from 'd3-shape';

/**
 * Animated streamgraph mark — stacked time series with wiggle baseline,
 * scrolled across a sliding window. Inspired by d3/streamgraph/2.
 *
 * Each category becomes one ribbon mesh. Triangle-strip vertices update
 * every frame with a Catmull-Rom-smoothed scroll offset so the front edge
 * of new data eases in instead of stepping.
 */

export interface StreamgraphSeries {
  /** Category label (used for color order; not rendered as text by the mesh). */
  category: string;
  /** Equal-length array of non-negative values (one per timestep). */
  values: number[];
}

export interface StreamgraphOptions {
  width?: number;
  height?: number;
  /** Number of timesteps visible at once. Default 60. */
  windowSize?: number;
  /** Timesteps per second the window scrolls. Default 8. */
  scrollSpeed?: number;
  /** Subdivisions between adjacent timesteps for visual smoothing. Default 6. */
  smoothing?: number;
  palette?: number[];
}

export interface StreamgraphViz {
  group: THREE.Group;
  /** Call every frame with seconds-since-start. */
  tick(time: number): void;
  /** Hand back the resolved category order (matches palette index order). */
  categories(): string[];
}

const DEFAULT_PALETTE = [
  0xff7a8a, 0xffb873, 0xffd97a, 0xf5e9c8, 0xcc7a99, 0xe89a6f, 0xa68a6a, 0xff9966,
];

export function buildStreamgraph(series: StreamgraphSeries[], opts: StreamgraphOptions = {}): StreamgraphViz {
  const {
    width = 0.36,
    height = 0.18,
    windowSize = 60,
    scrollSpeed = 8,
    smoothing = 6,
    palette = DEFAULT_PALETTE,
  } = opts;

  const g = new THREE.Group();
  g.name = 'streamgraph';

  const totalSteps = series[0]?.values.length ?? 0;
  if (totalSteps < 2 || series.length === 0) {
    return { group: g, tick: () => {}, categories: () => [] };
  }

  // Reshape for d3-stack: rows[t] = { cat1: v, cat2: v, ... }
  const rows: Array<Record<string, number>> = [];
  for (let t = 0; t < totalSteps; t++) {
    const row: Record<string, number> = {};
    for (const s of series) row[s.category] = s.values[t] ?? 0;
    rows.push(row);
  }

  const stackGen = stack<Record<string, number>>()
    .keys(series.map(s => s.category))
    .offset(stackOffsetWiggle)
    .order(stackOrderInsideOut);

  // d3 SeriesPoint is [number, number] with extra `data`/`key` props; just take the pair.
  const stacked = stackGen(rows).map(layer => layer.map(([y0, y1]) => [y0, y1] as const));

  // Global vertical extent across all layers / all timesteps.
  let globalMin = Infinity, globalMax = -Infinity;
  for (const layer of stacked) {
    for (const [y0, y1] of layer) {
      if (y0 < globalMin) globalMin = y0;
      if (y1 > globalMax) globalMax = y1;
    }
  }
  const span = globalMax - globalMin || 1;
  const yScale = height / span;
  const yOffset = -height / 2 - globalMin * yScale;

  // Each layer renders `(windowSize - 1) * smoothing` quads.
  const totalQuads = (windowSize - 1) * smoothing;
  const vertsPerQuad = 6; // 2 triangles, 3 verts each
  const stride = 3;       // x, y, z

  interface LayerRender {
    layer: ReadonlyArray<readonly [number, number]>;
    geo: THREE.BufferGeometry;
    positions: Float32Array;
  }

  const layers: LayerRender[] = stacked.map((layer, idx) => {
    const positions = new Float32Array(totalQuads * vertsPerQuad * stride);
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(positions, 3));

    // Lighting-independent — MeshStandardMaterial would collapse to ~black on
    // optical passthrough because per-frame position updates don't recompute
    // normals, so the diffuse term goes to zero and only emissive contributes.
    // MeshBasicMaterial renders the full colour regardless of normals/lighting.
    const color = palette[idx % palette.length]!;
    const mat = new THREE.MeshBasicMaterial({
      color,
      transparent: true, opacity: 0.92,
      side: THREE.DoubleSide,
    });
    g.add(new THREE.Mesh(geo, mat));
    return { layer, geo, positions };
  });

  // Catmull-Rom interpolation between layer[i] and layer[i+1] using neighbours i-1 and i+2 for tangents.
  function crom(p0: number, p1: number, p2: number, p3: number, t: number): number {
    const t2 = t * t;
    const t3 = t2 * t;
    return 0.5 * (
      (2 * p1) +
      (-p0 + p2) * t +
      (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
      (-p0 + 3 * p1 - 3 * p2 + p3) * t3
    );
  }

  function sampleAt(layer: LayerRender['layer'], idxFloat: number): readonly [number, number] {
    const i1 = Math.floor(idxFloat);
    const f = idxFloat - i1;
    const i0 = ((i1 - 1) % totalSteps + totalSteps) % totalSteps;
    const i2 = ((i1 + 1) % totalSteps + totalSteps) % totalSteps;
    const i3 = ((i1 + 2) % totalSteps + totalSteps) % totalSteps;
    const a = layer[((i1 % totalSteps) + totalSteps) % totalSteps]!;
    const b = layer[i2]!;
    const aPrev = layer[i0]!;
    const bNext = layer[i3]!;
    const y0 = crom(aPrev[0]!, a[0]!, b[0]!, bNext[0]!, f);
    const y1 = crom(aPrev[1]!, a[1]!, b[1]!, bNext[1]!, f);
    return [y0, y1] as const;
  }

  function writeWindow(scrollFloat: number) {
    for (const lr of layers) {
      const { layer, positions, geo } = lr;
      let v = 0;
      for (let q = 0; q < totalQuads; q++) {
        const u0 = q / totalQuads;
        const u1 = (q + 1) / totalQuads;
        const idxA = scrollFloat + u0 * (windowSize - 1);
        const idxB = scrollFloat + u1 * (windowSize - 1);
        const a = sampleAt(layer, idxA);
        const b = sampleAt(layer, idxB);

        const xA = u0 * width - width / 2;
        const xB = u1 * width - width / 2;
        const yA0 = a[0] * yScale + yOffset;
        const yA1 = a[1] * yScale + yOffset;
        const yB0 = b[0] * yScale + yOffset;
        const yB1 = b[1] * yScale + yOffset;

        // Triangle 1: (xA, yA0), (xA, yA1), (xB, yB1)
        positions[v++] = xA; positions[v++] = yA0; positions[v++] = 0;
        positions[v++] = xA; positions[v++] = yA1; positions[v++] = 0;
        positions[v++] = xB; positions[v++] = yB1; positions[v++] = 0;
        // Triangle 2: (xA, yA0), (xB, yB1), (xB, yB0)
        positions[v++] = xA; positions[v++] = yA0; positions[v++] = 0;
        positions[v++] = xB; positions[v++] = yB1; positions[v++] = 0;
        positions[v++] = xB; positions[v++] = yB0; positions[v++] = 0;
      }
      (geo.attributes.position as THREE.BufferAttribute).needsUpdate = true;
    }
  }

  function tick(time: number) {
    const scrollFloat = (time * scrollSpeed) % totalSteps;
    writeWindow(scrollFloat);
  }

  writeWindow(0);

  return { group: g, tick, categories: () => series.map(s => s.category) };
}
