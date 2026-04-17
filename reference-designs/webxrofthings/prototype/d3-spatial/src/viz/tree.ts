import * as THREE from 'three';
import { hierarchy, tree as d3tree, cluster, HierarchyPointNode } from 'd3-hierarchy';
import { Text } from 'troika-three-text';
import type { HNode } from '../demo/sampleHierarchy';
import { TEXT } from '../ui/palette';
import { tweenInstanced, easeExpOut } from '../util/tween';

export interface TreeOptions {
  form?: 'wall' | 'radial';
  radius?: number;
  size?: [number, number];
  nodeColor?: number;
  edgeColor?: number;
  selectColor?: number;
}

export interface TreeNodeInfo {
  name: string;
  depth: number;
  childCount: number;
  isLeaf: boolean;
  value?: number;
}

export interface TreeViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  getNodeInfo(i: number): TreeNodeInfo;
  setSelected(i: number, selected: boolean): void;
  toggleSelected(i: number): boolean;
  clearSelection(): void;
  getSelected(): Set<number>;
  drillIn(nodeIndex: number): boolean;
  drillOut(): boolean;
  getFocusPath(): number[];
  tick(): void;
}

export function buildTree(root: HNode, opts: TreeOptions = {}): TreeViz {
  const {
    form = 'radial',
    radius = 0.18,
    size = [0.3, 0.22],
    nodeColor = 0x66ccff,
    edgeColor = 0x3a5a8a,
    selectColor = 0x88ff99,
  } = opts;
  const g = new THREE.Group();
  g.name = `tree-${form}`;

  const fullHier = hierarchy(root);
  const allDescendants = fullHier.descendants();

  const toPos = (n: any): THREE.Vector3 => {
    if (form === 'radial') {
      const theta: number = n.x;
      const r: number = n.y;
      return new THREE.Vector3(Math.cos(theta - Math.PI / 2) * r, Math.sin(theta - Math.PI / 2) * r, 0);
    }
    return new THREE.Vector3(n.x - size[0] / 2, -(n.y - size[1] / 2), 0);
  };

  function layoutSubtree(subtreeRoot: HNode) {
    const hier = hierarchy(subtreeRoot);
    const layout = form === 'radial'
      ? cluster<HNode>().size([2 * Math.PI, radius])
      : d3tree<HNode>().size(size);
    layout(hier);
    return hier;
  }

  // Initial layout
  let currentHier = layoutSubtree(root);
  let descendants = currentHier.descendants();
  let positions: THREE.Vector3[] = descendants.map(n => toPos(n));
  let radii = descendants.map(n => n.children ? 0.006 : 0.004);

  const maxNodes = allDescendants.length;
  const focusPath: number[] = [];

  const edgeMat = new THREE.LineBasicMaterial({ color: edgeColor, transparent: true, opacity: 0.75 });
  let edgeGeo = new THREE.BufferGeometry();
  let edges = new THREE.LineSegments(edgeGeo, edgeMat);

  function rebuildEdges() {
    g.remove(edges);
    edgeGeo.dispose();
    const edgePoints: THREE.Vector3[] = [];
    const desc = descendants;
    currentHier.links().forEach(link => {
      const si = desc.indexOf(link.source as any);
      const ti = desc.indexOf(link.target as any);
      if (si >= 0 && ti >= 0 && positions[si] && positions[ti]) {
        edgePoints.push(positions[si]!);
        edgePoints.push(positions[ti]!);
      }
    });
    edgeGeo = new THREE.BufferGeometry().setFromPoints(edgePoints);
    edges = new THREE.LineSegments(edgeGeo, edgeMat);
    g.add(edges);
  }

  rebuildEdges();

  const nodeGeo = new THREE.SphereGeometry(1, 14, 14);
  const nodeMat = new THREE.MeshStandardMaterial({
    color: nodeColor, emissive: nodeColor, emissiveIntensity: 0.35, roughness: 0.4, metalness: 0.1,
  });
  const nodeMesh = new THREE.InstancedMesh(nodeGeo, nodeMat, maxNodes);
  nodeMesh.name = 'tree-nodes';
  nodeMesh.userData.isTreeNodes = true;

  const m = new THREE.Matrix4();

  function writeInstances() {
    for (let i = 0; i < descendants.length; i++) {
      const r = radii[i]!;
      m.compose(positions[i]!, new THREE.Quaternion(), new THREE.Vector3(r, r, r));
      nodeMesh.setMatrixAt(i, m);
    }
    // Hide unused instances
    for (let i = descendants.length; i < maxNodes; i++) {
      m.compose(new THREE.Vector3(0, 0, -100), new THREE.Quaternion(), new THREE.Vector3(0, 0, 0));
      nodeMesh.setMatrixAt(i, m);
    }
    nodeMesh.count = descendants.length;
    nodeMesh.instanceMatrix.needsUpdate = true;
  }

  writeInstances();
  g.add(nodeMesh);

  // Text labels — keep a set of them, rebuild on drill
  let textLabels: Text[] = [];
  function rebuildLabels() {
    for (const t of textLabels) { g.remove(t); t.dispose(); }
    textLabels = [];
    descendants.forEach((n, i) => {
      if (n.depth === 0 || n.depth > 2) return;
      const p = positions[i]!;
      const txt = new Text();
      txt.text = n.data.name;
      txt.fontSize = 0.009;
      txt.color = TEXT.body;
      txt.anchorX = 'left';
      txt.anchorY = 'middle';
      txt.position.set(p.x + 0.008, p.y, p.z);
      txt.sync();
      g.add(txt);
      textLabels.push(txt);
    });
  }
  rebuildLabels();

  const selectionMarkers = new Map<number, THREE.Mesh>();
  const markerGeo = new THREE.SphereGeometry(1, 20, 20);
  const markerMat = new THREE.MeshBasicMaterial({
    color: selectColor, transparent: true, opacity: 0.28,
    depthTest: false, side: THREE.BackSide,
  });

  let activeTween: { tick(): void; done: boolean } | null = null;

  function findAllDescIdx(node: { data: HNode; parent: any }): number {
    const path: string[] = [];
    let cur: { data: HNode; parent: any } | null = node;
    while (cur) { path.unshift(cur.data.name); cur = cur.parent; }
    for (let i = 0; i < allDescendants.length; i++) {
      const n = allDescendants[i]!;
      const nPath: string[] = [];
      let c: typeof n | null = n;
      while (c) { nPath.unshift(c.data.name); c = c.parent; }
      if (nPath.length === path.length && nPath.every((v, j) => v === path[j])) return i;
    }
    return -1;
  }

  function doRelayout(subtreeRoot: HNode) {
    const oldPositions = positions.map(p => p.clone());
    const oldRadii = [...radii];

    currentHier = layoutSubtree(subtreeRoot);
    descendants = currentHier.descendants();
    positions = descendants.map(n => toPos(n));
    radii = descendants.map(n => n.children ? 0.006 : 0.004);

    // Clear selection
    for (const [, marker] of selectionMarkers) g.remove(marker);
    selectionMarkers.clear();

    // Setup tween
    const count = Math.max(oldPositions.length, positions.length);
    const fromArr: { pos: THREE.Vector3; scale: number }[] = [];
    const toArr: { pos: THREE.Vector3; scale: number }[] = [];
    for (let i = 0; i < count; i++) {
      fromArr.push({
        pos: oldPositions[i]?.clone() ?? new THREE.Vector3(0, 0, -1),
        scale: oldRadii[i] ?? 0,
      });
      toArr.push({
        pos: positions[i]?.clone() ?? new THREE.Vector3(0, 0, -1),
        scale: radii[i] ?? 0,
      });
    }

    nodeMesh.count = count;
    activeTween = tweenInstanced(nodeMesh, fromArr, toArr, 500, easeExpOut);

    // Rebuild edges + labels after tween starts (they snap to new positions)
    rebuildEdges();
    rebuildLabels();
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
    setSelected: (i, selected) => {
      if (selected && !selectionMarkers.has(i)) {
        const marker = new THREE.Mesh(markerGeo, markerMat);
        marker.position.copy(positions[i]!);
        marker.scale.setScalar((radii[i] ?? 0.005) * 2.6);
        marker.renderOrder = 993;
        marker.userData.noHover = true;
        g.add(marker);
        selectionMarkers.set(i, marker);
      } else if (!selected && selectionMarkers.has(i)) {
        const marker = selectionMarkers.get(i)!;
        g.remove(marker);
        selectionMarkers.delete(i);
      }
    },
    toggleSelected: (i) => {
      const nowSel = !selectionMarkers.has(i);
      if (nowSel) {
        const marker = new THREE.Mesh(markerGeo, markerMat);
        marker.position.copy(positions[i]!);
        marker.scale.setScalar((radii[i] ?? 0.005) * 2.6);
        marker.renderOrder = 993;
        marker.userData.noHover = true;
        g.add(marker);
        selectionMarkers.set(i, marker);
      } else {
        const marker = selectionMarkers.get(i)!;
        g.remove(marker);
        selectionMarkers.delete(i);
      }
      return nowSel;
    },
    clearSelection: () => {
      for (const [, marker] of selectionMarkers) g.remove(marker);
      selectionMarkers.clear();
    },
    getSelected: () => new Set(selectionMarkers.keys()),
    drillIn: (nodeIndex: number) => {
      const node = descendants[nodeIndex];
      if (!node || !node.children || node.children.length === 0) return false;
      const idx = findAllDescIdx(node);
      if (idx >= 0) focusPath.push(idx);
      doRelayout(node.data);
      return true;
    },
    drillOut: () => {
      if (focusPath.length === 0) return false;
      focusPath.pop();
      if (focusPath.length === 0) {
        doRelayout(root);
      } else {
        const parentNode = allDescendants[focusPath[focusPath.length - 1]!]!;
        doRelayout(parentNode.data);
      }
      return true;
    },
    getFocusPath: () => [...focusPath],
    tick: () => {
      if (activeTween && !activeTween.done) {
        activeTween.tick();
        if (activeTween.done) activeTween = null;
      }
    },
  };
}
