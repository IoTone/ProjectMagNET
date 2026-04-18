import * as THREE from 'three';
import { Text } from 'troika-three-text';
import type { ParallelDataPoint } from '../demo/sampleHierarchy';
import { TEXT, EDGE } from '../ui/palette';

export interface ParallelViz {
  group: THREE.Group;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  tick(): void;
}

const LINE_COLORS = [0xff5577, 0x66ccff, 0xffcc66];

export function buildParallel(
  data: { dimensions: string[]; points: ParallelDataPoint[] },
  opts: { width?: number; height?: number } = {},
): ParallelViz {
  const { width = 0.28, height = 0.2 } = opts;
  const { dimensions, points } = data;
  const numAxes = dimensions.length;

  const g = new THREE.Group();
  g.name = 'parallel-coordinates';

  // Axis positions along x
  const axisSpacing = width / (numAxes - 1);
  const axisX: number[] = [];
  for (let a = 0; a < numAxes; a++) {
    axisX.push(-width / 2 + a * axisSpacing);
  }

  // Draw axes as thin cylinders
  const axisMat = new THREE.MeshStandardMaterial({
    color: EDGE.axis, roughness: 0.6, metalness: 0.1,
    transparent: true, opacity: 0.6,
  });
  for (let a = 0; a < numAxes; a++) {
    const axGeo = new THREE.CylinderGeometry(0.001, 0.001, height, 6);
    const ax = new THREE.Mesh(axGeo, axisMat);
    ax.position.set(axisX[a]!, 0, 0);
    ax.userData.noHover = true;
    g.add(ax);

    // Axis label at top
    const lbl = new Text();
    lbl.text = dimensions[a]!;
    lbl.fontSize = 0.009;
    lbl.color = TEXT.primary;
    lbl.anchorX = 'center';
    lbl.anchorY = 'bottom';
    lbl.position.set(axisX[a]!, height / 2 + 0.008, 0);
    lbl.sync();
    g.add(lbl);

    // Scale markers
    const mk0 = new Text();
    mk0.text = '0';
    mk0.fontSize = 0.006;
    mk0.color = TEXT.dim;
    mk0.anchorX = 'center';
    mk0.anchorY = 'top';
    mk0.position.set(axisX[a]!, -height / 2 - 0.004, 0);
    mk0.sync();
    g.add(mk0);

    const mk1 = new Text();
    mk1.text = '1';
    mk1.fontSize = 0.006;
    mk1.color = TEXT.dim;
    mk1.anchorX = 'center';
    mk1.anchorY = 'bottom';
    mk1.position.set(axisX[a]!, height / 2 - 0.002, 0);
    mk1.sync();
    g.add(mk1);
  }

  // Draw lines for each data point
  const linePositions: THREE.Vector3[][] = [];
  for (const pt of points) {
    const lineVerts: THREE.Vector3[] = [];
    for (let a = 0; a < numAxes; a++) {
      const v = pt.values[a] ?? 0.5;
      const y = -height / 2 + v * height;
      lineVerts.push(new THREE.Vector3(axisX[a]!, y, 0));
    }
    linePositions.push(lineVerts);

    const color = LINE_COLORS[pt.group % LINE_COLORS.length]!;
    const lineMat = new THREE.LineBasicMaterial({
      color, transparent: true, opacity: 0.65,
    });
    // Convert to pairs for LineSegments
    const segPts: THREE.Vector3[] = [];
    for (let i = 0; i < lineVerts.length - 1; i++) {
      segPts.push(lineVerts[i]!);
      segPts.push(lineVerts[i + 1]!);
    }
    const lineGeo = new THREE.BufferGeometry().setFromPoints(segPts);
    const line = new THREE.LineSegments(lineGeo, lineMat);
    line.userData.noHover = true;
    g.add(line);
  }

  // Intersection dots at each axis crossing for hover
  const dotPositions: THREE.Vector3[] = [];
  const dotLabels: string[] = [];
  for (let pi = 0; pi < points.length; pi++) {
    for (let a = 0; a < numAxes; a++) {
      const v = points[pi]!.values[a] ?? 0.5;
      const y = -height / 2 + v * height;
      dotPositions.push(new THREE.Vector3(axisX[a]!, y, 0));
      dotLabels.push(`${points[pi]!.id}: ${dimensions[a]} = ${v.toFixed(2)}`);
    }
  }

  return {
    group: g,
    nodeCount: () => points.length,
    getNodeWorldPosition: (i, out = new THREE.Vector3()) => {
      // Return center of point's line (middle axis)
      const midAxis = Math.floor(numAxes / 2);
      const verts = linePositions[i];
      if (!verts || !verts[midAxis]) return out.set(0, 0, 0);
      out.copy(verts[midAxis]!);
      g.localToWorld(out);
      return out;
    },
    getNodeLabel: (i) => points[i]?.id ?? '',
    tick: () => {},
  };
}
