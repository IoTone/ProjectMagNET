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

export interface SankeyData {
  nodes: Array<{ id: string; name: string; group: number }>;
  links: Array<{ source: string; target: string; value: number }>;
}

export function sampleSankey(): SankeyData {
  return {
    nodes: [
      { id: 'solar',   name: 'Solar',   group: 0 },
      { id: 'wind',    name: 'Wind',    group: 0 },
      { id: 'grid',    name: 'Grid',    group: 1 },
      { id: 'battery', name: 'Battery', group: 1 },
      { id: 'home',    name: 'Home',    group: 2 },
      { id: 'office',  name: 'Office',  group: 2 },
      { id: 'lights',  name: 'Lights',  group: 3 },
      { id: 'hvac',    name: 'HVAC',    group: 3 },
      { id: 'compute', name: 'Compute', group: 3 },
      { id: 'ev',      name: 'EV',      group: 3 },
    ],
    links: [
      { source: 'solar',   target: 'grid',    value: 40 },
      { source: 'solar',   target: 'battery', value: 15 },
      { source: 'wind',    target: 'grid',    value: 30 },
      { source: 'wind',    target: 'battery', value: 10 },
      { source: 'grid',    target: 'home',    value: 45 },
      { source: 'grid',    target: 'office',  value: 25 },
      { source: 'battery', target: 'home',    value: 15 },
      { source: 'battery', target: 'office',  value: 10 },
      { source: 'home',    target: 'lights',  value: 18 },
      { source: 'home',    target: 'hvac',    value: 25 },
      { source: 'home',    target: 'compute', value: 12 },
      { source: 'office',  target: 'lights',  value: 10 },
      { source: 'office',  target: 'compute', value: 15 },
      { source: 'office',  target: 'ev',      value: 10 },
    ],
  };
}

export function sampleTangles(): Array<{ source: string; target: string; type: string }> {
  return [
    { source: 'temp', target: 'lights', type: 'control' },
    { source: 'edge', target: 'wrist', type: 'sync' },
    { source: 'motion', target: 'hvac', type: 'control' },
    { source: 'cloud', target: 'glasses', type: 'sync' },
    { source: 'humidity', target: 'blinds', type: 'control' },
  ];
}

export interface ParallelDataPoint {
  id: string;
  group: number;
  values: number[]; // one per dimension
}

export function sampleParallel(): { dimensions: string[]; points: ParallelDataPoint[] } {
  const dimensions = ['temp', 'humidity', 'light', 'motion', 'power'];
  const rand = mulberry32(42);
  const points: ParallelDataPoint[] = [];
  for (let i = 0; i < 18; i++) {
    const group = Math.floor(rand() * 3);
    const base = [0.3 + group * 0.2, 0.5, 0.4 + group * 0.1, 0.6 - group * 0.15, 0.35 + group * 0.2];
    const values = base.map(b => Math.max(0, Math.min(1, b + (rand() - 0.5) * 0.4)));
    points.push({ id: `p${i}`, group, values });
  }
  return { dimensions, points };
}

export interface StreamgraphSample {
  category: string;
  values: number[];
}

export function sampleStreamgraph(numCategories = 6, numSteps = 200, seed = 11): StreamgraphSample[] {
  const rand = mulberry32(seed);
  const labels = ['Sensors', 'Compute', 'Network', 'Storage', 'Display', 'Audio', 'Battery', 'Radio'];
  const out: StreamgraphSample[] = [];
  for (let c = 0; c < numCategories; c++) {
    const phase = rand() * Math.PI * 2;
    const freq = 0.025 + rand() * 0.04;
    const amp = 8 + rand() * 14;
    const base = 14 + rand() * 6;
    const trendSlope = (rand() - 0.5) * 12;
    const values: number[] = [];
    for (let t = 0; t < numSteps; t++) {
      const wave = 0.5 + 0.5 * Math.sin(t * freq + phase);
      const trend = (t / numSteps) * trendSlope;
      const noise = (rand() - 0.5) * 1.5;
      values.push(Math.max(0, base + amp * wave + trend + noise));
    }
    out.push({ category: labels[c % labels.length]!, values });
  }
  return out;
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
