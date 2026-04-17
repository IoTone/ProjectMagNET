import * as THREE from 'three';
import { forceSimulation, forceManyBody, forceLink, forceCenter } from 'd3-force-3d';
import type { GraphData } from '../demo/sampleHierarchy';

export interface ForceOptions {
  size?: number;
  ticks?: number;
  palette?: number[];
}

interface SimNode {
  id: string; group: number;
  x?: number; y?: number; z?: number;
  fx?: number | null; fy?: number | null; fz?: number | null;
}
interface SimLink { source: string | SimNode; target: string | SimNode; value?: number; }

export interface ForceViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodes: SimNode[];
  tick(): void;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  pinNode(i: number, worldPos: THREE.Vector3): void;
  unpinNode(i: number): void;
  reheat(alpha?: number): void;
}

export function buildForceGraph(data: GraphData, opts: ForceOptions = {}): ForceViz {
  const {
    size = 0.22,
    ticks = 120,
    palette = [0xff5577, 0x66ccff, 0xffcc66, 0xcc99ff],
  } = opts;

  const group = new THREE.Group();
  group.name = 'force-graph';

  const nodes: SimNode[] = data.nodes.map(n => ({ ...n }));
  const links: SimLink[] = data.links.map(l => ({ ...l }));

  const sim = forceSimulation(nodes, 3)
    .force('charge', forceManyBody().strength(-14))
    .force('link', forceLink(links).id((d: any) => d.id).distance(0.03).strength(0.6))
    .force('center', forceCenter(0, 0, 0))
    .alpha(1)
    .alphaDecay(0.012)
    .stop();
  for (let i = 0; i < ticks; i++) sim.tick();

  const scale = computeScale(nodes, size);

  const nodeGeo = new THREE.SphereGeometry(1, 14, 14);
  const nodeMat = new THREE.MeshStandardMaterial({ roughness: 0.4, metalness: 0.1 });
  const nodeMesh = new THREE.InstancedMesh(nodeGeo, nodeMat, nodes.length);
  nodeMesh.name = 'force-nodes';
  nodeMesh.userData.isForceNodes = true;

  const colors = new Float32Array(nodes.length * 3);
  nodes.forEach((n, i) => {
    const col = new THREE.Color(palette[n.group % palette.length]!);
    colors[i * 3] = col.r;
    colors[i * 3 + 1] = col.g;
    colors[i * 3 + 2] = col.b;
  });
  nodeMesh.instanceColor = new THREE.InstancedBufferAttribute(colors, 3);
  writeInstanceMatrices(nodeMesh, nodes, scale);
  group.add(nodeMesh);

  const edgeGeo = new THREE.BufferGeometry();
  edgeGeo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(links.length * 2 * 3), 3));
  const edgeMat = new THREE.LineBasicMaterial({ color: 0x3a5a8a, transparent: true, opacity: 0.6 });
  const edges = new THREE.LineSegments(edgeGeo, edgeMat);
  edges.userData.isForceEdges = true;
  group.add(edges);

  function writeEdges() {
    const pos = edgeGeo.attributes.position as THREE.BufferAttribute;
    for (let i = 0; i < links.length; i++) {
      const l = links[i]!;
      const s = l.source as SimNode;
      const t = l.target as SimNode;
      pos.setXYZ(i * 2,     (s.x ?? 0) * scale, (s.y ?? 0) * scale, (s.z ?? 0) * scale);
      pos.setXYZ(i * 2 + 1, (t.x ?? 0) * scale, (t.y ?? 0) * scale, (t.z ?? 0) * scale);
    }
    pos.needsUpdate = true;
  }
  writeEdges();

  function tick() {
    sim.tick(1);
    writeInstanceMatrices(nodeMesh, nodes, scale);
    writeEdges();
  }

  function getNodeWorldPosition(i: number, out = new THREE.Vector3()): THREE.Vector3 {
    const n = nodes[i]!;
    out.set((n.x ?? 0) * scale, (n.y ?? 0) * scale, (n.z ?? 0) * scale);
    group.localToWorld(out);
    return out;
  }

  function pinNode(i: number, worldPos: THREE.Vector3): void {
    const local = group.worldToLocal(worldPos.clone());
    const n = nodes[i]!;
    n.fx = local.x / scale;
    n.fy = local.y / scale;
    n.fz = local.z / scale;
    n.x = n.fx;
    n.y = n.fy;
    n.z = n.fz;
    sim.alpha(Math.max(sim.alpha(), 0.3));
  }

  function unpinNode(i: number): void {
    const n = nodes[i]!;
    n.fx = null;
    n.fy = null;
    n.fz = null;
  }

  return {
    group,
    nodeMesh,
    nodes,
    tick,
    nodeCount: () => nodes.length,
    getNodeWorldPosition,
    getNodeLabel: (i: number) => nodes[i]?.id ?? '',
    pinNode,
    unpinNode,
    reheat: (a = 0.4) => sim.alpha(Math.max(sim.alpha(), a)),
  };
}

function computeScale(nodes: SimNode[], size: number): number {
  let maxExt = 0;
  for (const n of nodes) {
    maxExt = Math.max(maxExt, Math.abs(n.x ?? 0), Math.abs(n.y ?? 0), Math.abs(n.z ?? 0));
  }
  return maxExt > 0 ? (size / 2) / maxExt : 1;
}

function writeInstanceMatrices(mesh: THREE.InstancedMesh, nodes: SimNode[], scale: number) {
  const m = new THREE.Matrix4();
  const r = 0.005;
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i]!;
    m.compose(
      new THREE.Vector3((n.x ?? 0) * scale, (n.y ?? 0) * scale, (n.z ?? 0) * scale),
      new THREE.Quaternion(),
      new THREE.Vector3(r, r, r),
    );
    mesh.setMatrixAt(i, m);
  }
  mesh.instanceMatrix.needsUpdate = true;
}
