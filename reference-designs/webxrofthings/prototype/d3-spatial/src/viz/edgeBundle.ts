import * as THREE from 'three';
import { hierarchy, cluster } from 'd3-hierarchy';
import { Text } from 'troika-three-text';
import type { HNode, GraphData } from '../demo/sampleHierarchy';
import { TEXT, EDGE } from '../ui/palette';

export interface EdgeBundleViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  tick(): void;
}

const GROUP_COLORS = [0xff5577, 0x66ccff, 0xffcc66, 0xcc99ff, 0x66ffcc];

export function buildEdgeBundle(
  treeRoot: HNode,
  graph: GraphData,
  opts: { radius?: number; beta?: number } = {},
): EdgeBundleViz {
  const { radius = 0.12, beta = 0.85 } = opts;

  const g = new THREE.Group();
  g.name = 'edge-bundle';

  // Build hierarchy and layout leaves on a circle
  const hier = hierarchy(treeRoot);
  const lay = cluster<HNode>().size([2 * Math.PI, radius]);
  lay(hier);

  const leaves = hier.leaves();
  const allDescs = hier.descendants();
  const positions: THREE.Vector3[] = [];

  // Map each descendant to a position
  for (const n of allDescs) {
    const theta = (n as any).x as number;
    const r = (n as any).y as number;
    positions.push(new THREE.Vector3(
      Math.cos(theta - Math.PI / 2) * r,
      Math.sin(theta - Math.PI / 2) * r,
      0,
    ));
  }

  // Name -> descendant index
  const nameToIdx = new Map<string, number>();
  allDescs.forEach((n, i) => nameToIdx.set(n.data.name, i));

  // Build leaf name -> leaf index for graph mapping
  const leafNames = leaves.map(l => l.data.name);

  // Nodes (InstancedMesh) - only for leaves on the circle
  const nodeGeo = new THREE.SphereGeometry(1, 14, 14);
  const nodeMat = new THREE.MeshStandardMaterial({
    color: 0x66ccff, emissive: 0x66ccff, emissiveIntensity: 0.35,
    roughness: 0.4, metalness: 0.1,
  });
  const nodeMesh = new THREE.InstancedMesh(nodeGeo, nodeMat, allDescs.length);
  nodeMesh.name = 'edge-bundle-nodes';

  const mat = new THREE.Matrix4();
  const colors = new Float32Array(allDescs.length * 3);
  allDescs.forEach((n, i) => {
    const isLeaf = !n.children;
    const r = isLeaf ? 0.005 : 0.003;
    mat.compose(positions[i]!, new THREE.Quaternion(), new THREE.Vector3(r, r, r));
    nodeMesh.setMatrixAt(i, mat);
    // Color by depth group
    const col = new THREE.Color(GROUP_COLORS[n.depth % GROUP_COLORS.length]!);
    colors[i * 3] = col.r;
    colors[i * 3 + 1] = col.g;
    colors[i * 3 + 2] = col.b;
  });
  nodeMesh.instanceColor = new THREE.InstancedBufferAttribute(colors, 3);
  nodeMesh.instanceMatrix.needsUpdate = true;
  // Hide internal nodes
  allDescs.forEach((n, i) => {
    if (n.children) {
      mat.compose(new THREE.Vector3(0, 0, -100), new THREE.Quaternion(), new THREE.Vector3(0, 0, 0));
      nodeMesh.setMatrixAt(i, mat);
    }
  });
  nodeMesh.instanceMatrix.needsUpdate = true;
  nodeMesh.count = allDescs.length;
  g.add(nodeMesh);

  // Labels for leaves
  for (const leaf of leaves) {
    const idx = allDescs.indexOf(leaf);
    const p = positions[idx]!;
    const txt = new Text();
    txt.text = leaf.data.name;
    txt.fontSize = 0.006;
    txt.color = TEXT.body;
    const theta = (leaf as any).x as number;
    const labelR = radius + 0.012;
    txt.anchorX = (theta > Math.PI) ? 'right' : 'left';
    txt.anchorY = 'middle';
    txt.position.set(
      Math.cos(theta - Math.PI / 2) * labelR,
      Math.sin(theta - Math.PI / 2) * labelR,
      0,
    );
    txt.sync();
    g.add(txt);
  }

  // Path from a node to root
  function pathToRoot(node: typeof allDescs[0]): typeof allDescs[0][] {
    const path: typeof allDescs[0][] = [];
    let cur = node;
    while (cur) {
      path.push(cur);
      cur = cur.parent as typeof cur;
    }
    return path;
  }

  // Find the path through the hierarchy between two leaves
  function hierarchyPath(a: typeof allDescs[0], b: typeof allDescs[0]): typeof allDescs[0][] {
    const pathA = pathToRoot(a);
    const pathB = pathToRoot(b);
    // Find LCA
    const setA = new Set(pathA);
    let lca: typeof allDescs[0] | undefined;
    for (const n of pathB) {
      if (setA.has(n)) { lca = n; break; }
    }
    if (!lca) return [a, b];

    const upToLCA = pathA.slice(0, pathA.indexOf(lca) + 1);
    const downFromLCA = pathB.slice(0, pathB.indexOf(lca)).reverse();
    return [...upToLCA, ...downFromLCA];
  }

  // Create bundled edges from graph links
  // Map graph node IDs to tree leaf names (use modular mapping)
  const graphNodeToLeaf = new Map<string, string>();
  graph.nodes.forEach((gn, i) => {
    graphNodeToLeaf.set(gn.id, leafNames[i % leafNames.length]!);
  });

  // Limit edges to keep it readable
  const maxEdges = Math.min(graph.links.length, 25);
  for (let li = 0; li < maxEdges; li++) {
    const link = graph.links[li]!;
    const srcLeafName = graphNodeToLeaf.get(typeof link.source === 'string' ? link.source : (link.source as any).id);
    const tgtLeafName = graphNodeToLeaf.get(typeof link.target === 'string' ? link.target : (link.target as any).id);
    if (!srcLeafName || !tgtLeafName || srcLeafName === tgtLeafName) continue;

    const srcIdx = nameToIdx.get(srcLeafName);
    const tgtIdx = nameToIdx.get(tgtLeafName);
    if (srcIdx === undefined || tgtIdx === undefined) continue;

    const srcNode = allDescs[srcIdx]!;
    const tgtNode = allDescs[tgtIdx]!;

    const pathNodes = hierarchyPath(srcNode, tgtNode);
    if (pathNodes.length < 2) continue;

    // Build control points with bundling factor
    const controlPoints: THREE.Vector3[] = [];
    for (const pn of pathNodes) {
      const pi = allDescs.indexOf(pn);
      const pos = positions[pi]!;
      controlPoints.push(pos.clone());
    }

    // Apply beta bundling: interpolate each control point toward its straight-line position
    if (controlPoints.length > 2) {
      const start = controlPoints[0]!.clone();
      const end = controlPoints[controlPoints.length - 1]!.clone();
      for (let c = 1; c < controlPoints.length - 1; c++) {
        const straightT = c / (controlPoints.length - 1);
        const straightPos = new THREE.Vector3().lerpVectors(start, end, straightT);
        controlPoints[c]!.lerp(straightPos, 1 - beta);
      }
    }

    const curve = new THREE.CatmullRomCurve3(controlPoints);
    const tubeGeo = new THREE.TubeGeometry(curve, 16, 0.0003, 3, false);

    // Color by source group
    const srcGraphNode = graph.nodes.find(n => graphNodeToLeaf.get(n.id) === srcLeafName);
    const color = GROUP_COLORS[(srcGraphNode?.group ?? 0) % GROUP_COLORS.length]!;
    const tubeMat = new THREE.MeshStandardMaterial({
      color, emissive: color, emissiveIntensity: 0.3,
      transparent: true, opacity: 0.45,
      roughness: 0.5, metalness: 0.05,
    });
    const tube = new THREE.Mesh(tubeGeo, tubeMat);
    tube.userData.noHover = true;
    g.add(tube);
  }

  return {
    group: g,
    nodeMesh,
    nodeCount: () => allDescs.length,
    getNodeWorldPosition: (i, out = new THREE.Vector3()) => {
      const p = positions[i];
      if (!p) return out.set(0, 0, 0);
      out.copy(p);
      g.localToWorld(out);
      return out;
    },
    getNodeLabel: (i) => allDescs[i]?.data.name ?? '',
    tick: () => {},
  };
}
