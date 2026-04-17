import * as THREE from 'three';
import { hierarchy, partition, HierarchyRectangularNode } from 'd3-hierarchy';
import type { HNode } from '../demo/sampleHierarchy';
import { tweenMeshes, easeExpOut } from '../util/tween';

export interface SunburstOptions {
  innerRadius?: number;
  outerRadius?: number;
  thickness?: number;
  palette?: number[];
  selectColor?: number;
}

export interface SunburstSegmentInfo {
  name: string;
  depth: number;
  childCount: number;
  isLeaf: boolean;
  value?: number;
}

export interface SunburstViz {
  group: THREE.Group;
  segments: THREE.Mesh[];
  segmentCount(): number;
  getSegmentWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getSegmentLabel(i: number): string;
  getSegmentInfo(i: number): SunburstSegmentInfo;
  toggleSelected(i: number): boolean;
  clearSelection(): void;
  getSelected(): Set<number>;
  drillIn(segmentIndex: number): boolean;
  drillOut(): boolean;
  getFocusPath(): number[];
  getFocusLabels(): string[];
  tick(): void;
}

export function buildSunburst(root: HNode, opts: SunburstOptions = {}): SunburstViz {
  const {
    innerRadius = 0.03,
    outerRadius = 0.16,
    thickness = 0.01,
    palette = [0xff5577, 0x66ccff, 0xffcc66, 0xcc99ff, 0x66ffcc, 0xff99cc],
    selectColor = 0x88ff99,
  } = opts;

  const g = new THREE.Group();
  g.name = 'sunburst';

  const fullHier = hierarchy<HNode>(root).sum(d => (d as any).value ?? 0).sort((a, b) => (b.value! - a.value!));
  const allDescendants = fullHier.descendants();

  const focusPath: number[] = [];
  const segments: THREE.Mesh[] = [];
  const segmentPositions: THREE.Vector3[] = [];
  let segmentNodes: HierarchyRectangularNode<HNode>[] = [];

  function buildSegments(subtreeRoot: HNode) {
    // Clear existing
    for (const m of segments) { g.remove(m); m.geometry.dispose(); (m.material as THREE.Material).dispose(); }
    segments.length = 0;
    segmentPositions.length = 0;
    segmentNodes.length = 0;

    const hier = hierarchy<HNode>(subtreeRoot).sum(d => (d as any).value ?? 0).sort((a, b) => (b.value! - a.value!));
    partition<HNode>().size([2 * Math.PI, outerRadius - innerRadius])(hier);

    const desc = hier.descendants();
    desc.forEach((n, idx) => {
      if (n.depth === 0) return;
      const any = n as any;
      const a0: number = any.x0;
      const a1: number = any.x1;
      const r0: number = innerRadius + (any.y0 - 0);
      const r1: number = innerRadius + any.y1;
      const zOffset = (n.depth - 1) * thickness * 1.6;

      const shape = new THREE.Shape();
      const segs = Math.max(8, Math.floor((a1 - a0) * 24));
      shape.absarc(0, 0, r1, a0 - Math.PI / 2, a1 - Math.PI / 2, false);
      shape.absarc(0, 0, r0, a1 - Math.PI / 2, a0 - Math.PI / 2, true);
      const geo = new THREE.ExtrudeGeometry(shape, { depth: thickness, bevelEnabled: false, curveSegments: segs });
      const color = palette[idx % palette.length]!;
      const mat = new THREE.MeshStandardMaterial({
        color, emissive: color, emissiveIntensity: 0.2,
        roughness: 0.45, metalness: 0.1,
        transparent: true, opacity: 0.88,
        side: THREE.DoubleSide,
      });
      const mesh = new THREE.Mesh(geo, mat);
      mesh.position.z = zOffset;
      mesh.userData.segmentIndex = segments.length;
      g.add(mesh);

      // Segment centroid for hover position
      const midAngle = (a0 + a1) / 2 - Math.PI / 2;
      const midR = (r0 + r1) / 2;
      segmentPositions.push(new THREE.Vector3(
        Math.cos(midAngle) * midR,
        Math.sin(midAngle) * midR,
        zOffset + thickness / 2,
      ));
      segmentNodes.push(n as HierarchyRectangularNode<HNode>);
      segments.push(mesh);
    });
  }

  buildSegments(root);

  // Selection markers
  const selectionMarkers = new Map<number, THREE.Mesh>();
  const markerMat = new THREE.MeshBasicMaterial({
    color: selectColor, transparent: true, opacity: 0.32,
    depthTest: false, side: THREE.BackSide,
  });

  // Tween state
  let activeTween: { tick(): void; done: boolean } | null = null;

  function findAllDescIdx(node: HierarchyRectangularNode<HNode>): number {
    const path: string[] = [];
    let cur: HierarchyRectangularNode<HNode> | null = node;
    while (cur) { path.unshift(cur.data.name); cur = cur.parent as HierarchyRectangularNode<HNode> | null; }
    for (let i = 0; i < allDescendants.length; i++) {
      const n = allDescendants[i]!;
      const nPath: string[] = [];
      let c: typeof n | null = n;
      while (c) { nPath.unshift(c.data.name); c = c.parent; }
      if (nPath.length === path.length && nPath.every((v, j) => v === path[j])) return i;
    }
    return -1;
  }

  return {
    group: g,
    segments,
    segmentCount: () => segments.length,
    getSegmentWorldPosition: (i, out = new THREE.Vector3()) => {
      const p = segmentPositions[i];
      if (!p) return out.set(0, 0, 0);
      out.copy(p);
      g.localToWorld(out);
      return out;
    },
    getSegmentLabel: (i) => segmentNodes[i]?.data.name ?? '',
    getSegmentInfo: (i) => {
      const n = segmentNodes[i];
      return {
        name: n?.data.name ?? '',
        depth: n?.depth ?? 0,
        childCount: n?.children?.length ?? 0,
        isLeaf: !n?.children,
        value: n?.value,
      };
    },
    toggleSelected: (i) => {
      const nowSel = !selectionMarkers.has(i);
      if (nowSel) {
        const p = segmentPositions[i];
        if (!p) return false;
        const markerGeo = new THREE.SphereGeometry(0.012, 16, 16);
        const marker = new THREE.Mesh(markerGeo, markerMat);
        marker.position.copy(p);
        marker.renderOrder = 993;
        marker.userData.noHover = true;
        g.add(marker);
        selectionMarkers.set(i, marker);
      } else {
        const marker = selectionMarkers.get(i)!;
        g.remove(marker);
        marker.geometry.dispose();
        selectionMarkers.delete(i);
      }
      return nowSel;
    },
    clearSelection: () => {
      for (const [, m] of selectionMarkers) { g.remove(m); m.geometry.dispose(); }
      selectionMarkers.clear();
    },
    getSelected: () => new Set(selectionMarkers.keys()),
    drillIn: (segmentIndex: number) => {
      const node = segmentNodes[segmentIndex];
      if (!node || !node.children || node.children.length === 0) return false;

      const oldPositions = segments.map(m => m.position.clone());
      const oldScales = segments.map(m => m.scale.clone());

      const idx = findAllDescIdx(node);
      if (idx >= 0) focusPath.push(idx);

      // Clear selection
      for (const [, m] of selectionMarkers) { g.remove(m); m.geometry.dispose(); }
      selectionMarkers.clear();

      buildSegments(node.data);

      // Tween from old center to new positions
      const fromArr = oldPositions.map((p, i) => ({
        pos: p,
        scale: oldScales[i] ?? new THREE.Vector3(1, 1, 1),
      }));
      const toArr = segments.map((m, i) => ({
        pos: segmentPositions[i]?.clone() ?? new THREE.Vector3(),
        scale: m.scale.clone(),
      }));
      // For tweenMeshes we need position-based interpolation
      // Since we rebuilt geometry, just animate scale from 0 to 1
      for (const m of segments) m.scale.setScalar(0.01);
      const scaleFrom = segments.map(() => ({ pos: new THREE.Vector3(0, 0, 0), scale: new THREE.Vector3(0.01, 0.01, 0.01) }));
      const scaleTo = segments.map((m, i) => ({ pos: segmentPositions[i]?.clone() ?? new THREE.Vector3(), scale: new THREE.Vector3(1, 1, 1) }));
      // Reset positions so tween can animate
      segments.forEach((m, i) => { m.position.set(0, 0, 0); });
      activeTween = tweenMeshes(segments, scaleFrom, scaleTo, 500, easeExpOut);
      return true;
    },
    drillOut: () => {
      if (focusPath.length === 0) return false;
      focusPath.pop();

      for (const [, m] of selectionMarkers) { g.remove(m); m.geometry.dispose(); }
      selectionMarkers.clear();

      if (focusPath.length === 0) {
        buildSegments(root);
      } else {
        const parentNode = allDescendants[focusPath[focusPath.length - 1]!]!;
        buildSegments(parentNode.data);
      }

      // Animate scale in
      for (const m of segments) m.scale.setScalar(0.01);
      const scaleFrom = segments.map(() => ({ pos: new THREE.Vector3(0, 0, 0), scale: new THREE.Vector3(0.01, 0.01, 0.01) }));
      const scaleTo = segments.map((m, i) => ({ pos: segmentPositions[i]?.clone() ?? new THREE.Vector3(), scale: new THREE.Vector3(1, 1, 1) }));
      segments.forEach((m, i) => { m.position.set(0, 0, 0); });
      activeTween = tweenMeshes(segments, scaleFrom, scaleTo, 500, easeExpOut);
      return true;
    },
    getFocusPath: () => [...focusPath],
    getFocusLabels: () => {
      const labels = ['root'];
      for (const idx of focusPath) {
        const n = allDescendants[idx];
        if (n) labels.push(n.data.name);
      }
      return labels;
    },
    tick: () => {
      if (activeTween && !activeTween.done) {
        activeTween.tick();
        if (activeTween.done) activeTween = null;
      }
    },
  };
}
