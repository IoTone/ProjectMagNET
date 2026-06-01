import * as THREE from 'three';
import { hierarchy, tree as d3tree } from 'd3-hierarchy';
import { Text } from 'troika-three-text';
import type { HNode } from '../demo/sampleHierarchy';
import { TEXT, EDGE } from '../ui/palette';

export interface TidyTreeViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  getNodeInfo(i: number): { name: string; depth: number; childCount: number; isLeaf: boolean; value?: number };
  tick(): void;
}

export function buildTidyTree(root: HNode, opts: { cylinderRadius?: number; height?: number } = {}): TidyTreeViz {
  const {
    cylinderRadius = 0.08,
    height = 0.22,
  } = opts;

  const g = new THREE.Group();
  g.name = 'tidy-tree-cylindrical';

  const hier = hierarchy(root);
  const layout = d3tree<HNode>().size([2 * Math.PI, height]);
  layout(hier);

  const descendants = hier.descendants();
  const positions: THREE.Vector3[] = [];

  // Map (x, y) -> cylindrical surface: theta = x, height_y = y
  for (const n of descendants) {
    const theta = (n as any).x as number;
    const h = (n as any).y as number;
    const px = Math.cos(theta) * cylinderRadius;
    const py = height / 2 - h;
    const pz = Math.sin(theta) * cylinderRadius;
    positions.push(new THREE.Vector3(px, py, pz));
  }

  // Edges
  const edgePoints: THREE.Vector3[] = [];
  hier.links().forEach(link => {
    const si = descendants.indexOf(link.source);
    const ti = descendants.indexOf(link.target);
    if (si >= 0 && ti >= 0 && positions[si] && positions[ti]) {
      edgePoints.push(positions[si]!);
      edgePoints.push(positions[ti]!);
    }
  });
  const edgeGeo = new THREE.BufferGeometry().setFromPoints(edgePoints);
  const edgeMat = new THREE.LineBasicMaterial({ color: EDGE.link, transparent: true, opacity: 0.7 });
  const edges = new THREE.LineSegments(edgeGeo, edgeMat);
  edges.userData.noHover = true;
  g.add(edges);

  // Nodes (InstancedMesh)
  const nodeGeo = new THREE.SphereGeometry(1, 14, 14);
  const nodeMat = new THREE.MeshStandardMaterial({
    color: 0x66ccff, emissive: 0x66ccff, emissiveIntensity: 0.35,
    roughness: 0.4, metalness: 0.1,
  });
  const nodeMesh = new THREE.InstancedMesh(nodeGeo, nodeMat, descendants.length);
  nodeMesh.name = 'tidy-tree-nodes';

  const mat = new THREE.Matrix4();
  descendants.forEach((n, i) => {
    const r = n.children ? 0.006 : 0.005;
    mat.compose(positions[i]!, new THREE.Quaternion(), new THREE.Vector3(r, r, r));
    nodeMesh.setMatrixAt(i, mat);
  });
  nodeMesh.instanceMatrix.needsUpdate = true;
  g.add(nodeMesh);

  // Labels for depth 0-2
  descendants.forEach((n, i) => {
    if (n.depth > 2) return;
    const p = positions[i]!;
    const txt = new Text();
    txt.text = n.data.name;
    txt.fontSize = 0.008;
    txt.color = TEXT.body;
    txt.anchorX = 'left';
    txt.anchorY = 'middle';
    txt.position.set(p.x + 0.008, p.y, p.z);
    txt.sync();
    g.add(txt);
  });

  return {
    group: g,
    nodeMesh,
    nodeCount: () => descendants.length,
    getNodeWorldPosition: (i, out = new THREE.Vector3()) => {
      const p = positions[i];
      if (!p) return out.set(0, 0, 0);
      out.copy(p);
      g.localToWorld(out);
      return out;
    },
    getNodeLabel: (i) => descendants[i]?.data.name ?? '',
    getNodeInfo: (i) => {
      const n = descendants[i];
      return {
        name: n?.data.name ?? '',
        depth: n?.depth ?? 0,
        childCount: n?.children?.length ?? 0,
        isLeaf: !n?.children,
        value: (n?.data as { value?: number }).value,
      };
    },
    tick: () => {},
  };
}
