/*
 * forceTree3d — d3-force-3d simulation of a small hierarchical tree,
 * shaped to behave EXACTLY like the gallery's force-directed graph:
 *
 *   - per-node grab (pinNode / unpinNode) — the user pinches a node
 *     and pulls it through 3D space; the rest of the tree follows the
 *     spring forces.
 *   - no rotation-on-click. The earlier version toggled an auto-spin
 *     on select, which is what the user explicitly DOESN'T want.
 *
 * Reference: gallery force.ts (src/viz/force.ts). This module keeps
 * the same `ForceViz`-shaped surface (group, nodeMesh, nodes, tick,
 * nodeCount, getNodeWorldPosition, getNodeLabel, pinNode, unpinNode,
 * reheat) so renderManifest can register it with the same Interact
 * drag callbacks the gallery uses — no special-casing for trees.
 *
 * The simulation self-ticks via nodeMesh.onBeforeRender (the manifest
 * pipeline doesn't drive an external per-mark tick loop the way main.ts
 * does for the gallery). The loop early-outs once alpha settles, so a
 * quiescent tree costs nothing per frame.
 */

import * as THREE from 'three';
import { forceSimulation, forceManyBody, forceLink, forceCenter } from 'd3-force-3d';

export interface ForceTree3dOptions {
  /** Number of cluster sub-trees hanging off the root. Default 5. */
  clusters?: number;
  /** Leaves per cluster (depth-2 nodes). Default 5. */
  leavesPerCluster?: number;
  /** Overall bbox extent in metres. Default 0.40. */
  size?: number;
  /** Node sphere radius in metres before fit-scaling. Default 0.012 —
   *  +25 % over the standard force-graph node (≈ 0.0095). */
  radius?: number;
  /** Initial pre-tick iterations on construction. Default 40 — enough
   *  to get the cluster sub-trees off the random initial positions and
   *  into a plausible shape, while keeping the sync cost on Quest-class
   *  hardware under ~80 ms (160 ticks, the previous default, hung the
   *  UC3 entry by ~300–800 ms on Quest 3). The onBeforeRender loop is
   *  alpha-gated and keeps ticking past construction, so the layout
   *  finishes settling visibly over the first ~1.5 s — reads as a
   *  "tree growing in" rather than a hang. */
  ticks?: number;
  /** Cluster colour palette (one entry per cluster, cycled if shorter). */
  palette?: number[];
}

interface SimNode {
  id: string;
  depth: number;       /* 0 = root, 1 = cluster, 2 = leaf */
  cluster: number;     /* 0..clusters-1 */
  x?: number; y?: number; z?: number;
  fx?: number | null; fy?: number | null; fz?: number | null;
}
interface SimLink { source: string | SimNode; target: string | SimNode; }

export interface ForceTree3dViz {
  group: THREE.Group;
  /** Exposed for Interact + main.ts to pick / drag instances on. */
  nodeMesh: THREE.InstancedMesh;
  /** Exposed read-only so the same drag wiring as the gallery
   *  force-graph (which iterates `viz.nodes`) works unmodified. */
  nodes: SimNode[];
  tick(): void;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  pinNode(i: number, worldPos: THREE.Vector3): void;
  unpinNode(i: number): void;
  reheat(alpha?: number): void;
  dispose(): void;
}

export function buildForceTree3d(opts: ForceTree3dOptions = {}): ForceTree3dViz {
  const {
    clusters         = 5,
    leavesPerCluster = 5,
    size             = 0.40,
    radius           = 0.012,
    ticks            = 40,
    palette          = [0xff5577, 0x66ccff, 0xffcc66, 0xcc99ff, 0x77ddaa],
  } = opts;

  const group = new THREE.Group();
  group.name = 'force-tree-3d';

  /* ─── Generate tree topology ──────────────────────────────────── */
  const nodes: SimNode[] = [];
  const links: SimLink[] = [];

  nodes.push({ id: 'root', depth: 0, cluster: 0 });
  for (let c = 0; c < clusters; c++) {
    const cid = `c${c}`;
    nodes.push({ id: cid, depth: 1, cluster: c });
    links.push({ source: 'root', target: cid });
    for (let l = 0; l < leavesPerCluster; l++) {
      const lid = `c${c}-l${l}`;
      nodes.push({ id: lid, depth: 2, cluster: c });
      links.push({ source: cid, target: lid });
    }
  }

  /* ─── Force simulation ────────────────────────────────────────── */
  /* Plain force-directed-tree (after Bostock's @d3/force-directed-tree
   * notebook): manyBody repulsion + link springs + center-pull. No
   * per-depth Y bias — the cluster sub-trees radiate organically. */
  const sim = forceSimulation(nodes, 3)
    .force('charge', forceManyBody().strength(-10))
    .force('link',   forceLink(links).id((d: any) => d.id).distance(0.05).strength(0.7))
    .force('center', forceCenter(0, 0, 0))
    .alpha(1)
    .alphaDecay(0.012)
    .stop();
  for (let i = 0; i < ticks; i++) sim.tick();

  /* ─── Fit-scale to bbox ───────────────────────────────────────── */
  /* Computed once from the post-settle layout. Live ticks operate in
   * simulation coords; we scale at render time so the visible tree
   * stays within `size` even as the user drags a node out. */
  let minX =  Infinity, maxX = -Infinity, minY =  Infinity, maxY = -Infinity, minZ =  Infinity, maxZ = -Infinity;
  for (const n of nodes) {
    const x = n.x ?? 0, y = n.y ?? 0, z = n.z ?? 0;
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
  }
  const span = Math.max(maxX - minX, maxY - minY, maxZ - minZ) || 1;
  const fit  = size / span;

  /* ─── Node spheres (instanced) ────────────────────────────────── */
  /* Geometry is baked at `radius` directly (rather than unit-sphere +
   * per-instance scale) so the InstancedMesh's first-pass bounding-
   * sphere cull stays tight without depending on per-instance matrix
   * scale being read correctly by THREE's raycaster. The previous
   * unit-sphere + scale variant turned out to miss raycasts on this
   * config — the user reported a beam that visibly shortens before
   * reaching the tree, consistent with the cull rejecting the mesh. */
  const nodeGeo = new THREE.SphereGeometry(radius, 18, 16);
  const nodeMat = new THREE.MeshStandardMaterial({ roughness: 0.4, metalness: 0.1 });
  const nodeMesh = new THREE.InstancedMesh(nodeGeo, nodeMat, nodes.length);
  nodeMesh.name = 'tree-nodes';
  /* Don't frustum-cull the InstancedMesh. The XR controller raycast
   * runs before WebGLRenderer's frustum pass, but a stale `frustumCulled`
   * flag can still skip the picking on the first few frames after the
   * mesh is added — observed as "the tree won't accept raycasts" on
   * UC3. The cost is negligible (~32 instances). */
  nodeMesh.frustumCulled = false;

  const colors = new Float32Array(nodes.length * 3);
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i]!;
    const col = new THREE.Color(palette[n.cluster % palette.length]!);
    colors[i * 3]     = col.r;
    colors[i * 3 + 1] = col.g;
    colors[i * 3 + 2] = col.b;
  }
  nodeMesh.instanceColor = new THREE.InstancedBufferAttribute(colors, 3);
  group.add(nodeMesh);

  /* Edge segments — one pair of vertices per link. Updated in
   * writeEdges() alongside the node matrices on every tick. */
  const edgeGeo = new THREE.BufferGeometry();
  edgeGeo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(links.length * 2 * 3), 3));
  const edgeMat = new THREE.LineBasicMaterial({ color: 0xb8a380, transparent: true, opacity: 0.6 });
  const edges = new THREE.LineSegments(edgeGeo, edgeMat);
  group.add(edges);

  /* ─── Soft fill light — the standard scene has minimal ambient and
   * the tree would otherwise read as flat-shaded silhouettes. */
  const fill = new THREE.DirectionalLight(0xffffff, 1.4);
  fill.position.set(0.3, 0.6, 0.4);
  group.add(fill);
  const ambient = new THREE.AmbientLight(0xffffff, 0.25);
  group.add(ambient);

  /* ─── Write helpers ───────────────────────────────────────────── */
  function writeMatrices() {
    const m = new THREE.Matrix4();
    for (let i = 0; i < nodes.length; i++) {
      const n = nodes[i]!;
      /* Root + cluster nodes are slightly bigger so the hierarchy
       * reads at a glance (parents = chunkier). The geometry is
       * already baked at `radius`, so the per-instance scale acts as
       * a size *multiplier* on top — 1.0 for leaves, 1.25 for
       * clusters, 1.6 for the root. */
      const sizeMul = n.depth === 0 ? 1.6 : n.depth === 1 ? 1.25 : 1.0;
      m.compose(
        new THREE.Vector3((n.x ?? 0) * fit, (n.y ?? 0) * fit, (n.z ?? 0) * fit),
        new THREE.Quaternion(),
        new THREE.Vector3(sizeMul, sizeMul, sizeMul),
      );
      nodeMesh.setMatrixAt(i, m);
    }
    nodeMesh.instanceMatrix.needsUpdate = true;
    /* Recompute the InstancedMesh's bounding sphere from current
     * instance positions, then deliberately INFLATE the radius. The
     * THREE raycaster's first-pass cull is a sphere intersect; if our
     * tight sphere is even slightly out-of-sync with the rendered
     * instance positions (e.g. during the few frames where the sim
     * is still relaxing), the cull rejects and no instance is
     * tested. Inflating the radius lets per-instance hits resolve
     * correctly without the cull dropping anything early. */
    nodeMesh.computeBoundingSphere();
    if (nodeMesh.boundingSphere) {
      nodeMesh.boundingSphere.radius *= 2.5;
    }
  }

  function writeEdges() {
    const pos = edgeGeo.attributes.position as THREE.BufferAttribute;
    const nodeById = new Map(nodes.map(n => [n.id, n] as const));
    for (let i = 0; i < links.length; i++) {
      const l = links[i]!;
      const s = typeof l.source === 'string' ? nodeById.get(l.source)! : l.source as SimNode;
      const t = typeof l.target === 'string' ? nodeById.get(l.target)! : l.target as SimNode;
      pos.setXYZ(i * 2,     (s.x ?? 0) * fit, (s.y ?? 0) * fit, (s.z ?? 0) * fit);
      pos.setXYZ(i * 2 + 1, (t.x ?? 0) * fit, (t.y ?? 0) * fit, (t.z ?? 0) * fit);
    }
    pos.needsUpdate = true;
  }

  writeMatrices();
  writeEdges();

  /* ─── Live tick ───────────────────────────────────────────────── */
  /* The manifest pipeline doesn't call a per-mark tick from main.ts
   * (the gallery does, but renderManifest doesn't). Drive ticks from
   * onBeforeRender so a settled tree costs nothing, but a grabbed
   * tree continues to relax while the user drags. */
  function tick() {
    sim.tick(1);
    writeMatrices();
    writeEdges();
  }

  let disposed = false;
  nodeMesh.onBeforeRender = () => {
    if (disposed) return;
    /* Skip when the sim is settled. d3-force's alpha decays toward
     * alphaMin (0.001 by default); below ~0.005 the visible motion is
     * imperceptible and not worth the per-frame cost. pinNode() bumps
     * alpha back up so grabs always animate. */
    if (sim.alpha() < 0.005) return;
    tick();
  };

  /* ─── Public API ──────────────────────────────────────────────── */
  function getNodeWorldPosition(i: number, out = new THREE.Vector3()): THREE.Vector3 {
    const n = nodes[i]!;
    out.set((n.x ?? 0) * fit, (n.y ?? 0) * fit, (n.z ?? 0) * fit);
    group.localToWorld(out);
    return out;
  }

  function pinNode(i: number, worldPos: THREE.Vector3): void {
    /* Map the grabbed world point back through the cell's transform
     * stack into simulation coordinates, pin the node there, and
     * reheat so the rest of the tree starts moving to follow. */
    const local = group.worldToLocal(worldPos.clone());
    const n = nodes[i]!;
    n.fx = local.x / fit;
    n.fy = local.y / fit;
    n.fz = local.z / fit;
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
    dispose() {
      disposed = true;
      nodeMesh.onBeforeRender = () => {};
      nodeGeo.dispose();
      nodeMat.dispose();
      edgeGeo.dispose();
      edgeMat.dispose();
    },
  };
}
