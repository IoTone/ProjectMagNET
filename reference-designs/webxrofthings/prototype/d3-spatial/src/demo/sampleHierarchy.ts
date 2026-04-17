export interface HNode {
  name: string;
  value?: number;
  children?: HNode[];
}

export const sampleTree: HNode = {
  name: 'root',
  children: [
    { name: 'sensors', children: [
      { name: 'temp', value: 12 },
      { name: 'humidity', value: 8 },
      { name: 'motion', value: 14 },
      { name: 'light', value: 6 },
    ]},
    { name: 'actuators', children: [
      { name: 'lights', value: 20 },
      { name: 'blinds', value: 7 },
      { name: 'hvac', value: 24 },
    ]},
    { name: 'compute', children: [
      { name: 'gateway', value: 18 },
      { name: 'edge', value: 22 },
      { name: 'cloud', value: 10 },
    ]},
    { name: 'wearables', children: [
      { name: 'wrist', value: 9 },
      { name: 'ring', value: 4 },
      { name: 'glasses', value: 15 },
    ]},
  ],
};

export interface GraphData {
  nodes: Array<{ id: string; group: number }>;
  links: Array<{ source: string; target: string; value?: number }>;
}

export function sampleGraph(n = 30, seed = 7): GraphData {
  const rand = mulberry32(seed);
  const nodes = Array.from({ length: n }, (_, i) => ({
    id: `n${i}`,
    group: Math.floor(rand() * 4),
  }));
  const links: GraphData['links'] = [];
  for (let i = 1; i < n; i++) {
    const j = Math.floor(rand() * i);
    links.push({ source: `n${i}`, target: `n${j}`, value: 1 });
  }
  for (let k = 0; k < n * 0.6; k++) {
    const a = Math.floor(rand() * n);
    const b = Math.floor(rand() * n);
    if (a !== b) links.push({ source: `n${a}`, target: `n${b}`, value: 1 });
  }
  return { nodes, links };
}

export function sampleRidgeline(rows = 6, samples = 60, seed = 3): number[][] {
  const rand = mulberry32(seed);
  const series: number[][] = [];
  for (let r = 0; r < rows; r++) {
    const mode1 = 0.2 + r * 0.1 + rand() * 0.1;
    const mode2 = 0.6 + rand() * 0.2;
    const w1 = 0.05 + rand() * 0.03;
    const w2 = 0.07 + rand() * 0.04;
    const row: number[] = [];
    for (let i = 0; i < samples; i++) {
      const x = i / (samples - 1);
      const v = gauss(x, mode1, w1) * (0.6 + 0.4 * rand()) + gauss(x, mode2, w2) * (0.3 + 0.3 * rand());
      row.push(v);
    }
    series.push(row);
  }
  return series;
}

function gauss(x: number, m: number, s: number) {
  const d = (x - m) / s;
  return Math.exp(-0.5 * d * d) / (s * Math.sqrt(2 * Math.PI));
}

function mulberry32(seed: number) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
