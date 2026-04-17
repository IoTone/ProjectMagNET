import * as THREE from 'three';
import { scaleLinear, scaleTime } from 'd3-scale';
import { Text } from 'troika-three-text';
import { Chart } from '../chart/Chart';
import { syntheticHR } from './heartRate';

export interface DemoMark {
  id: string;
  title: string;
  group: THREE.Group;
  chart: Chart;
}

export function buildDemoScene(): { root: THREE.Group; marks: DemoMark[] } {
  const root = new THREE.Group();
  root.name = 'demo-marks';
  const marks: DemoMark[] = [];

  const CELL_W = 0.32;
  const CELL_H = 0.22;
  const GAP = 0.05;

  const placements: [number, number][] = [
    [-(CELL_W + GAP) / 2, (CELL_H + GAP) / 2],
    [(CELL_W + GAP) / 2, (CELL_H + GAP) / 2],
    [-(CELL_W + GAP) / 2, -(CELL_H + GAP) / 2],
    [(CELL_W + GAP) / 2, -(CELL_H + GAP) / 2],
  ];

  const specs = [lineDemo(), barDemo(), scatterDemo(), arcDemo()];
  specs.forEach((spec, i) => {
    const [cx, cy] = placements[i]!;
    const cell = new THREE.Group();
    cell.position.set(cx - CELL_W / 2, cy - CELL_H / 2, 0);
    cell.add(spec.chart.object3D);

    const hitW = CELL_W * 1.1;
    const hitH = CELL_H * 1.2;
    const hitPlane = new THREE.Mesh(
      new THREE.PlaneGeometry(hitW, hitH),
      new THREE.MeshBasicMaterial({ visible: false, side: THREE.DoubleSide }),
    );
    hitPlane.position.set(CELL_W / 2, CELL_H / 2 + 0.02, -0.003);
    hitPlane.userData.isMarkHitPlane = true;
    cell.add(hitPlane);

    const title = new Text();
    title.text = spec.title;
    title.fontSize = 0.016;
    title.color = spec.color;
    title.anchorX = 'left';
    title.anchorY = 'bottom';
    title.position.set(0, CELL_H + 0.005, 0);
    title.sync();
    cell.add(title);

    root.add(cell);
    marks.push({ id: spec.id, title: spec.title, group: cell, chart: spec.chart });
  });

  return { root, marks };
}

function lineDemo() {
  const data = syntheticHR(60, 4, 42);
  const x = scaleTime().domain([data[0]!.t, data.at(-1)!.t]).range([0, 0.3]);
  const y = scaleLinear().domain([50, 110]).range([0, 0.18]);
  const chart = new Chart({
    xAccessor: (d: any) => d.t,
    yAccessor: (d: any) => d.v,
    markOptions: { color: 0xff5577, radius: 0.003 },
  })
    .x(x as any).y(y as any).mark('line').data(data as any).render();
  return { id: 'line', title: 'line · HR ribbon', color: 0xff5577, chart };
}

function barDemo() {
  const data = Array.from({ length: 12 }, (_, i) => ({
    t: i,
    v: 18 + Math.sin(i / 1.5) * 4 + Math.cos(i / 2.3) * 2,
  }));
  const x = scaleLinear().domain([0, 11]).range([0.015, 0.3 - 0.015]);
  const y = scaleLinear().domain([12, 26]).range([0, 0.18]);
  const chart = new Chart({
    xAccessor: (d: any) => d.t,
    yAccessor: (d: any) => d.v,
    markOptions: { color: 0x66ccff, width: 0.014, depth: 0.01 },
    baseline: 0,
  })
    .x(x as any).y(y as any).mark('bar').data(data as any).render();
  return { id: 'bar', title: 'bar · room temp', color: 0x66ccff, chart };
}

function scatterDemo() {
  const rand = mulberry32(7);
  const data = Array.from({ length: 180 }, () => ({
    t: rand() * 1,
    v: rand() * 1,
  }));
  const x = scaleLinear().domain([0, 1]).range([0.01, 0.3 - 0.01]);
  const y = scaleLinear().domain([0, 1]).range([0.01, 0.18 - 0.01]);
  const chart = new Chart({
    xAccessor: (d: any) => d.t,
    yAccessor: (d: any) => d.v,
    markOptions: { color: 0xffcc66, radius: 0.004 },
  })
    .x(x as any).y(y as any).mark('scatter').data(data as any).render();
  return { id: 'scatter', title: 'scatter · poster points', color: 0xffcc66, chart };
}

function arcDemo() {
  const x = scaleLinear().domain([0, 1]).range([0, 0.3]);
  const y = scaleLinear().domain([0, 1]).range([0, 0.18]);
  const chart = new Chart({
    xAccessor: () => 0,
    yAccessor: () => 0,
    markOptions: { color: 0xcc99ff, radius: 0.09, tubeRadius: 0.003 },
    showAxes: false,
  })
    .x(x as any).y(y as any).mark('arc').data([{ x: 0, y: 0 }] as any).render();
  chart.object3D.position.set(0.15, 0.11, 0);
  return { id: 'arc', title: 'arc · breadcrumb', color: 0xcc99ff, chart };
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
