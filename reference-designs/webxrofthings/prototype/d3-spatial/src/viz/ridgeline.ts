import * as THREE from 'three';

export interface RidgelineOptions {
  width?: number;
  rowHeight?: number;
  depthStep?: number;
  palette?: number[];
}

export interface RidgelineViz {
  group: THREE.Group;
  tick(time: number): void;
}

export function buildRidgeline(series: number[][], opts: RidgelineOptions = {}): RidgelineViz {
  const {
    width = 0.3,
    rowHeight = 0.04,
    depthStep = 0.02,
    palette = [0xff5577, 0xff99cc, 0xcc99ff, 0x66ccff, 0x66ffcc, 0xffcc66],
  } = opts;

  const g = new THREE.Group();
  g.name = 'ridgeline';

  const maxV = Math.max(...series.flat());
  const rows = series.length;

  // Store per-row data for animation
  const rowData: Array<{
    samples: number[];
    y0: number;
    z: number;
    fillGeo: THREE.BufferGeometry;
    fillPositions: Float32Array;
    lineGeo: THREE.BufferGeometry;
    linePositions: Float32Array;
    numSamples: number;
  }> = [];

  series.forEach((row, rIdx) => {
    const color = palette[rIdx % palette.length]!;
    const z = -rIdx * depthStep;
    const y0 = rows * 0.012 - rIdx * 0.02;
    const numSamples = row.length;

    // --- Fill ribbon using BufferGeometry with triangles ---
    // Each pair of adjacent samples forms a quad (2 triangles) between the baseline and the curve
    const numQuads = numSamples - 1;
    const fillVertCount = numQuads * 6; // 2 triangles per quad, 3 vertices each
    const fillPositions = new Float32Array(fillVertCount * 3);
    const fillGeo = new THREE.BufferGeometry();
    fillGeo.setAttribute('position', new THREE.BufferAttribute(fillPositions, 3));

    const fillMat = new THREE.MeshStandardMaterial({
      color, emissive: color, emissiveIntensity: 0.35,
      roughness: 0.5, metalness: 0.05,
      transparent: true, opacity: 0.85,
      side: THREE.DoubleSide,
    });
    g.add(new THREE.Mesh(fillGeo, fillMat));

    // --- Line on top ---
    const linePositions = new Float32Array(numSamples * 3);
    const lineGeo = new THREE.BufferGeometry();
    lineGeo.setAttribute('position', new THREE.BufferAttribute(linePositions, 3));

    const line = new THREE.Line(
      lineGeo,
      new THREE.LineBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.9 }),
    );
    g.add(line);

    rowData.push({
      samples: row,
      y0, z,
      fillGeo, fillPositions,
      lineGeo, linePositions,
      numSamples,
    });

    // Write initial positions (time=0)
    updateRow(rIdx, 0);
  });

  function updateRow(rIdx: number, time: number) {
    const rd = rowData[rIdx]!;
    const { samples, y0, z, fillPositions, linePositions, numSamples, fillGeo, lineGeo } = rd;
    const phase = Math.sin(time * 0.5 + rIdx * 0.3);

    for (let i = 0; i < numSamples; i++) {
      const baseX = (i / (numSamples - 1)) * width - width / 2;
      // Apply time-based offset: shift the sample index with wrapping
      const offset = phase * 3; // shift by up to 3 sample positions
      const srcIdx = ((i + Math.round(offset)) % numSamples + numSamples) % numSamples;
      const v = samples[srcIdx]!;
      const x = baseX;
      const y = y0 + (v / maxV) * rowHeight;

      // Line
      linePositions[i * 3] = x;
      linePositions[i * 3 + 1] = y;
      linePositions[i * 3 + 2] = z;

      // Fill quads: for each pair (i, i+1), write two triangles
      if (i < numSamples - 1) {
        const nextSrcIdx = ((i + 1 + Math.round(offset)) % numSamples + numSamples) % numSamples;
        const nextV = samples[nextSrcIdx]!;
        const nextX = ((i + 1) / (numSamples - 1)) * width - width / 2;
        const nextY = y0 + (nextV / maxV) * rowHeight;

        const base = i * 18; // 6 vertices * 3 components
        // Triangle 1: bottom-left, top-left, top-right
        fillPositions[base]     = x;     fillPositions[base + 1]  = y0; fillPositions[base + 2]  = z;
        fillPositions[base + 3] = x;     fillPositions[base + 4]  = y;  fillPositions[base + 5]  = z;
        fillPositions[base + 6] = nextX; fillPositions[base + 7]  = nextY; fillPositions[base + 8] = z;
        // Triangle 2: bottom-left, top-right, bottom-right
        fillPositions[base + 9]  = x;     fillPositions[base + 10] = y0;    fillPositions[base + 11] = z;
        fillPositions[base + 12] = nextX; fillPositions[base + 13] = nextY; fillPositions[base + 14] = z;
        fillPositions[base + 15] = nextX; fillPositions[base + 16] = y0;    fillPositions[base + 17] = z;
      }
    }

    (lineGeo.attributes.position as THREE.BufferAttribute).needsUpdate = true;
    (fillGeo.attributes.position as THREE.BufferAttribute).needsUpdate = true;
  }

  function tick(time: number) {
    for (let r = 0; r < rowData.length; r++) {
      updateRow(r, time);
    }
  }

  return { group: g, tick };
}
