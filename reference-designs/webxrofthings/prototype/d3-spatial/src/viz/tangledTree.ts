import * as THREE from 'three';
import { hierarchy, tree as d3tree } from 'd3-hierarchy';
import { Text } from 'troika-three-text';
import type { HNode } from '../demo/sampleHierarchy';
import { TEXT, EDGE } from '../ui/palette';

export interface TangledTreeViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  getNodeInfo(i: number): { name: string; depth: number; childCount: number; isLeaf: boolean; value?: number };
  tick(): void;
}

const TANGLE_COLORS: Record<string, number> = {
  control: 0xff5577,
  sync: 0x66ccff,
};

export function buildTangledTree(
  root: HNode,
  tangles: Array<{ source: string; target: string; type: string }>,
  opts: { width?: number; height?: number } = {},
): TangledTreeViz {
  const { width = 0.3, height = 0.22 } = opts;

  const g = new THREE.Group();
  g.name = 'tangled-tree';

  const hier = hierarchy(root);
  const layout = d3tree<HNode>().size([width, height]);
  layout(hier);

  const descendants = hier.descendants();
  const positions: THREE.Vector3[] = [];

  for (const n of descendants) {
    const x = (n as any).x as number;
    const y = (n as any).y as number;
    positions.push(new THREE.Vector3(x - width / 2, height / 2 - y, 0));
  }

  // Tree spine edges
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

  // Nodes
  const nodeGeo = new THREE.SphereGeometry(1, 14, 14);
  const nodeMat = new THREE.MeshStandardMaterial({
    color: 0xffcc66, emissive: 0xffcc66, emissiveIntensity: 0.35,
    roughness: 0.4, metalness: 0.1,
  });
  const nodeMesh = new THREE.InstancedMesh(nodeGeo, nodeMat, descendants.length);
  nodeMesh.name = 'tangled-tree-nodes';

  const mat = new THREE.Matrix4();
  descendants.forEach((n, i) => {
    const r = n.children ? 0.007 : 0.005;
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

  // Build name -> index map
  const nameMap = new Map<string, number>();
  descendants.forEach((n, i) => nameMap.set(n.data.name, i));

  // Tangle arcs
  for (const tangle of tangles) {
    const si = nameMap.get(tangle.source);
    const ti = nameMap.get(tangle.target);
    if (si === undefined || ti === undefined) continue;
    const sp = positions[si]!;
    const tp = positions[ti]!;

    const sNode = descendants[si]!;
    const tNode = descendants[ti]!;
    const levelSpan = Math.abs(sNode.depth - tNode.depth) + 1;
    const arcDepth = levelSpan * 0.02;

    const mid = new THREE.Vector3().addVectors(sp, tp).multiplyScalar(0.5);
    const curve = new THREE.CatmullRomCurve3([
      sp.clone(),
      new THREE.Vector3(sp.x + (tp.x - sp.x) * 0.25, sp.y + (tp.y - sp.y) * 0.25, arcDepth * 0.6),
      new THREE.Vector3(mid.x, mid.y, arcDepth),
      new THREE.Vector3(tp.x - (tp.x - sp.x) * 0.25, tp.y - (tp.y - sp.y) * 0.25, arcDepth * 0.6),
      tp.clone(),
    ]);

    const color = TANGLE_COLORS[tangle.type] ?? 0xcc99ff;
    const tubeGeo = new THREE.TubeGeometry(curve, 20, 0.0012, 4, false);
    const tubeMat = new THREE.MeshStandardMaterial({
      color, emissive: color, emissiveIntensity: 0.4,
      transparent: true, opacity: 0.7,
      roughness: 0.5, metalness: 0.05,
    });
    const tube = new THREE.Mesh(tubeGeo, tubeMat);
    tube.userData.noHover = true;
    g.add(tube);
  }

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
