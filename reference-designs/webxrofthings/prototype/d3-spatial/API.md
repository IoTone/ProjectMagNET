# d3-spatial API Reference

## Quick start

```bash
npm install
npm run dev          # Vite dev server at http://localhost:5173
# Open in browser — desktop interaction works; for XR use a tunnel + headset
```

---

## Core concepts

| Concept | Module | Purpose |
|---------|--------|---------|
| **Chart** | `src/chart/Chart.ts` | Fluent API for 2D-in-3D marks (line, bar, scatter, arc) with d3-scale. |
| **Viz** | `src/viz/*.ts` | Builder functions for spatial hierarchy/graph marks (tree, treemap, sunburst, pack, force, ridgeline, sankey). |
| **Interact** | `src/interact/Interact.ts` | Unified hover/select/drag state machine for desktop mouse and XR controllers. |
| **Manifest** | `src/manifest/*.ts` | Schema and loader for data-driven viz instantiation from a dataspace manifest. |

---

## Chart API

`src/chart/Chart.ts`

```ts
type MarkType = 'line' | 'bar' | 'scatter' | 'arc';

interface ChartDatum { [key: string]: unknown }

interface ChartOptions {
  xAccessor?: (d: ChartDatum) => number;
  yAccessor?: (d: ChartDatum) => number;
  markOptions?: LineMarkOptions | BarMarkOptions | ScatterMarkOptions | ArcMarkOptions;
  baseline?: number;
  showAxes?: boolean;
}

interface BrushResult {
  indices: number[];
  count: number;
  xRange: [number, number];
}
```

### `class Chart`

```ts
constructor(opts?: ChartOptions)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `x` | `(scale: ScaleFn) => this` | Set the x scale (any d3-scale-compatible function). |
| `y` | `(scale: ScaleFn) => this` | Set the y scale. |
| `mark` | `(type: MarkType) => this` | Set the mark type. |
| `data` | `(arr: ChartDatum[]) => this` | Set the data array. |
| `render` | `() => this` | Build marks, axes, and brush plane into `object3D`. |
| `updateData` | `(newData: ChartDatum[]) => void` | Live-update: rescales domains, rebuilds geometry. Line and bar marks tween smoothly. |
| `brush` | `(xFrac0: number, xFrac1: number) => BrushResult` | Programmatic brush: draws a cyan rectangle and returns selected indices. Fractions are 0..1 over the x domain. |
| `clearBrush` | `() => void` | Remove the brush rectangle. |
| `xFractionAt` | `(localX: number) => number` | Convert a local-space x coordinate to a 0..1 fraction. |
| `dispose` | `() => void` | Dispose all geometry and materials. |

**Properties:**

- `object3D: THREE.Group` — the root Group to parent into a scene.
- `xScale`, `yScale`, `dataset`, `markType` — read-only accessors.

---

## Viz builders

### `buildTree(root, opts?) -> TreeViz`

`src/viz/tree.ts`

```ts
interface TreeOptions {
  form?: 'wall' | 'radial';     // default: 'radial'
  radius?: number;               // default: 0.18
  size?: [number, number];       // default: [0.3, 0.22]
  nodeColor?: number;            // default: 0x66ccff
  edgeColor?: number;            // default: 0x3a5a8a
  selectColor?: number;          // default: 0x88ff99
}

interface TreeViz {
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
  getFocusLabels(): string[];
  tick(): void;                  // call each frame to advance drill-in tweens
}
```

### `buildTreemap(root, opts?) -> TreemapViz`

`src/viz/treemap.ts`

```ts
interface TreemapOptions {
  size?: [number, number];       // default: [0.3, 0.22]
  maxExtrude?: number;           // default: 0.05
  palette?: number[];
  selectColor?: number;
}

interface TreemapViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  getNodeInfo(i: number): TreemapNodeInfo;
  toggleSelected(i: number): boolean;
  clearSelection(): void;
  getSelected(): Set<number>;
  drillIn(nodeIndex: number): boolean;    // drills into the leaf's parent
  drillOut(): boolean;
  getFocusPath(): number[];
  getFocusLabels(): string[];
  tick(): void;
}
```

### `buildSunburst(root, opts?) -> SunburstViz`

`src/viz/sunburst.ts`

```ts
interface SunburstOptions {
  innerRadius?: number;          // default: 0.03
  outerRadius?: number;          // default: 0.16
  thickness?: number;            // default: 0.01
  palette?: number[];
  selectColor?: number;
}

interface SunburstViz {
  group: THREE.Group;
  segments: THREE.Mesh[];        // individual meshes, not instanced
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
```

### `buildCircularPack(root, opts?) -> PackViz`

`src/viz/pack.ts`

```ts
interface PackOptions {
  diameter?: number;             // default: 0.24
  palette?: number[];
  selectColor?: number;
}

interface PackViz {
  group: THREE.Group;
  spheres: THREE.Mesh[];         // individual meshes per node
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  getNodeInfo(i: number): PackNodeInfo;
  toggleSelected(i: number): boolean;
  clearSelection(): void;
  getSelected(): Set<number>;
  drillIn(nodeIndex: number): boolean;
  drillOut(): boolean;
  getFocusPath(): number[];
  getFocusLabels(): string[];
  tick(): void;
}
```

### `buildForceGraph(data, opts?) -> ForceViz`

`src/viz/force.ts`

```ts
interface ForceOptions {
  size?: number;                 // default: 0.22
  ticks?: number;                // warmup ticks, default: 120
  palette?: number[];
}

interface ForceViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodes: SimNode[];              // live simulation nodes with x/y/z/fx/fy/fz
  tick(): void;                  // advance physics one step + update meshes
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  pinNode(i: number, worldPos: THREE.Vector3): void;     // set fx/fy/fz, reheat
  unpinNode(i: number): void;                             // clear fx/fy/fz
  reheat(alpha?: number): void;                           // default: 0.4
}
```

Input data shape: `{ nodes: Array<{id, group}>, links: Array<{source, target, value?}> }`.

### `buildRidgeline(series, opts?) -> RidgelineViz`

`src/viz/ridgeline.ts`

```ts
interface RidgelineOptions {
  width?: number;                // default: 0.3
  rowHeight?: number;            // default: 0.04
  depthStep?: number;            // default: 0.02
  palette?: number[];
}

interface RidgelineViz {
  group: THREE.Group;
  tick(time: number): void;      // pass elapsed seconds for animation
}
```

Input: `number[][]` — each inner array is one row of density samples.

### `buildSankey(data) -> SankeyViz`

`src/viz/sankey.ts`

```ts
interface SankeyViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  getNodeInfo(i: number): { name: string; value: number; group: number };
}
```

Input data shape: `{ nodes: Array<{id, name, group}>, links: Array<{source, target, value}> }`.

### `buildTidyTree(root, opts?) -> TidyTreeViz`

`src/viz/tidyTree.ts`

```ts
interface TidyTreeOptions {
  cylinderRadius?: number;       // default: 0.08
  height?: number;               // default: 0.22
}

interface TidyTreeViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  getNodeInfo(i: number): { name: string; depth: number; childCount: number; isLeaf: boolean; value?: number };
  tick(): void;
}
```

Reingold-Tilford tree layout (via `d3-hierarchy` tree) mapped onto a cylindrical surface. Theta = x, height = y. Nodes as InstancedMesh spheres (parent 6mm, leaf 5mm), LineSegments edges in `EDGE.link`, troika labels for depth 0-2.

### `buildTangledTree(root, tangles, opts?) -> TangledTreeViz`

`src/viz/tangledTree.ts`

```ts
interface TangledTreeOptions {
  width?: number;                // default: 0.3
  height?: number;               // default: 0.22
}

interface TangledTreeViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  getNodeInfo(i: number): { name: string; depth: number; childCount: number; isLeaf: boolean; value?: number };
  tick(): void;
}
```

Standard tree spine (d3 tree layout, flat) with z-separated CatmullRom tangle arcs overlaid. Tangle colors: `control` = `0xff5577` (red), `sync` = `0x66ccff` (blue). Arc depth proportional to level span between source and target. Tube radius 1.2 mm.

Input `tangles`: `Array<{ source: string; target: string; type: string }>` — node names referencing the tree. Use `sampleTangles()` from `sampleHierarchy.ts`.

### `buildParallel(data, opts?) -> ParallelViz`

`src/viz/parallel.ts`

```ts
interface ParallelOptions {
  width?: number;                // default: 0.28
  height?: number;               // default: 0.2
}

interface ParallelViz {
  group: THREE.Group;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  tick(): void;
}
```

Parallel coordinates with vertical axis rods (CylinderGeometry, 1mm radius), axis labels at top (`TEXT.primary`, fontSize 0.009), and scale markers (0/1) at bottom. Data lines drawn as LineSegments colored by group (`0xff5577`, `0x66ccff`, `0xffcc66`). `nodeCount()` returns number of data points (not dots).

Input: `{ dimensions: string[]; points: ParallelDataPoint[] }` where `ParallelDataPoint = { id: string; group: number; values: number[] }`. Use `sampleParallel()`.

### `buildEdgeBundle(treeRoot, graph, opts?) -> EdgeBundleViz`

`src/viz/edgeBundle.ts`

```ts
interface EdgeBundleOptions {
  radius?: number;               // default: 0.12
  beta?: number;                 // bundling factor, default: 0.85
}

interface EdgeBundleViz {
  group: THREE.Group;
  nodeMesh: THREE.InstancedMesh;
  nodeCount(): number;
  getNodeWorldPosition(i: number, out?: THREE.Vector3): THREE.Vector3;
  getNodeLabel(i: number): string;
  tick(): void;
}
```

Hierarchical edge bundling. Leaves arranged on a circle via `d3-cluster`. Graph links are routed through the tree's LCA path, then bundled with factor `beta` (1.0 = full bundling, 0.0 = straight lines). Edges rendered as CatmullRom TubeGeometry (radius 0.3mm), colored by source group. Max 25 edges for readability. Per-instance color on nodes by depth.

Input: `treeRoot: HNode` (hierarchy) + `graph: GraphData` (nodes/links to overlay).

### `buildMorphDemo(rootData) -> MorphDemo`

`src/demo/morphDemo.ts`

```ts
interface MorphDemo {
  group: THREE.Group;
  tick(): void;                  // call each frame; advances active tween
  nextLayout(): void;            // cycle to the next layout type
  currentType(): string;         // current layout type name
  dispose(): void;               // clean up all geometry + materials
}
```

Single InstancedMesh that cycles through four hierarchy layouts: `tree` (radial cluster), `sunburst` (ring), `treemap` (area), `pack` (circle packing). Transitions use `tweenInstanced` with 800 ms duration and `easeExpOut` easing. Edges are hidden during the morph and rebuilt on completion. Per-instance color by depth. Layout label (troika Text) updates on each transition.

Layout cycle: `tree -> sunburst -> treemap -> pack -> tree -> ...`

---

## Interaction system

### `class Interact`

`src/interact/Interact.ts`

```ts
constructor(camera: THREE.Camera, canvas: HTMLCanvasElement, renderer?: THREE.WebGLRenderer)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `add` | `(h: Hoverable) => void` | Register a hoverable object. |
| `remove` | `(id: string) => void` | Unregister by id. |
| `setXrControllers` | `(controllers: THREE.Group[]) => void` | Wire XR controller ray sources. |
| `update` | `() => void` | Call each frame. Raycasts from active input source, updates hover state. |
| `hoverById` | `(id: string \| null) => void` | Programmatic hover (mouse hand slot). |
| `getHoveredId` | `() => string \| null` | Most-recently-hovered id from any hand. |
| `getHoveredIdForHand` | `(handIndex: number) => string \| null` | Hovered id for a specific hand (0, 1 = XR; 2 = mouse). |
| `getHoveredInstanceForHand` | `(handIndex: number) => number \| null` | Hovered instance id for a hand. |
| `triggerSelectForHand` | `(handIndex: number) => boolean` | Fire `onSelect` on the hovered item. |
| `triggerSelectOnHovered` | `() => boolean` | Fire `onSelect` on mouse-hand hover. |
| `beginDragForHand` | `(handIndex: number) => boolean` | Start drag if hovered item supports it. |
| `endDragForHand` | `(handIndex: number) => void` | End drag for hand. |
| `isDraggingForHand` | `(handIndex: number) => boolean` | True if this hand is dragging. |
| `isDragging` | `() => boolean` | True if any hand is dragging. |
| `setPressLockedForHand` | `(handIndex: number, locked: boolean) => void` | Lock hover target during a press. |
| `setExitDebounceMs` | `(ms: number) => void` | Set exit-hover debounce window. Default 150 ms. |

Legacy shortcuts: `beginDrag()`, `endDrag()`, `setPressLocked()` operate on the mouse hand slot (index 2).

### `hoverFeedback(obj: THREE.Object3D)`

Returns `{ on(), off() }` — applies 1.05x scale, +0.6 emissive, white outline box.

### `interface Hoverable`

```ts
interface Hoverable {
  id: string;
  object: THREE.Object3D;
  supportsInstances?: boolean;
  onHoverIn: (ctx?: HoverContext) => void;
  onHoverOut: (ctx?: HoverContext) => void;
  onHoverInstance?: (instanceId: number | null, handIndex?: number) => void;
  onSelect?: (ctx?: HoverContext) => void;
  onDragStart?: (ctx: HoverContext) => boolean | void;
  onDragMove?: (worldPoint: THREE.Vector3, handIndex?: number) => void;
  onDragEnd?: (ctx?: HoverContext) => void;
}
```

### `interface HoverContext`

```ts
interface HoverContext {
  instanceId?: number;
  worldPoint?: THREE.Vector3;
  distance?: number;
  hitObject?: THREE.Object3D;
  handIndex?: number;             // 0, 1 = XR hands; 2 = mouse
}
```

### `class FingertipGrab`

`src/interact/FingertipGrab.ts`

```ts
constructor(
  getHandState: (i: number) => HandJointState | null,
  findNearestNode: (worldPos: THREE.Vector3) => { index: number; distance: number } | null,
  events: FingertipGrabEvents,
)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `update` | `() => void` | Call each frame. Checks pinch distance and proximity for each hand. |
| `isGrabbing` | `(handIndex: number) => boolean` | True if this hand is currently grabbing. |
| `getGrabbedNode` | `(handIndex: number) => number \| null` | Index of grabbed node, or null. |

Thresholds: `GRAB_RADIUS = 0.025 m`, `PINCH_THRESHOLD = 0.02 m`.

Events:

```ts
interface FingertipGrabEvents {
  onGrab?: (handIndex: number, nearestNodeIndex: number, worldPos: THREE.Vector3) => void;
  onMove?: (handIndex: number, worldPos: THREE.Vector3) => void;
  onRelease?: (handIndex: number) => void;
  onProximity?: (handIndex: number, nearestNodeIndex: number, distance: number) => void;
  onProximityEnd?: (handIndex: number) => void;
}
```

### `class XRBrush`

`src/interact/XRBrush.ts`

```ts
constructor(events: XRBrushEvents)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `startBrush` | `(handIndex: number) => void` | Begin sweep-select for a hand. |
| `addToSelection` | `(handIndex: number, nodeIndex: number) => void` | Add a node to the active brush. |
| `endBrush` | `(handIndex: number) => void` | End brush and fire `onBrushEnd` with selected indices. |
| `isBrushing` | `(handIndex: number) => boolean` | True if brush is active. |
| `getSelected` | `(handIndex: number) => number[]` | Current selection. |

### `class DragBrush`

`src/interact/DragBrush.ts`

Desktop pointer-drag brush over charts.

```ts
constructor(
  camera: THREE.Camera,
  canvas: HTMLCanvasElement,
  charts: () => Chart[],
  events?: DragBrushEvents,
)
```

Events:

```ts
interface DragBrushEvents {
  onStart?: (chart: Chart) => void;
  onUpdate?: (chart: Chart, res: BrushResult) => void;
  onEnd?: (chart: Chart, res: BrushResult) => void;
  onClear?: () => void;
}
```

Property: `dragging: boolean` — true while a drag is in progress.

---

## UI components

### `class InspectorCard`

`src/ui/InspectorCard.ts`

```ts
constructor()
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `show` | `(content: InspectorContent) => void` | Display card with title, subtitle, value. |
| `hide` | `() => void` | Hide the card. |
| `placeNear` | `(target: THREE.Object3D, opts?: { preferredSide?, gap? }) => void` | Position adjacent to target. Auto-flips to clear side. |

```ts
interface InspectorContent {
  title: string;
  subtitle?: string;
  value?: string;
}
```

Property: `block` — the three-mesh-ui Block to parent into a scene.

### `class NodeHoverFx`

`src/ui/NodeHoverFx.ts`

Shared halo sphere + billboard label that follows the currently-hovered node. One instance per hand slot.

```ts
constructor(camera: THREE.Camera)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `show` | `(worldPos: THREE.Vector3, label: string, radius?: number) => void` | Show halo at position with label. |
| `hide` | `() => void` | Hide halo and label. |
| `updatePosition` | `(worldPos: THREE.Vector3) => void` | Move without changing label. |
| `tick` | `() => void` | Call each frame to update position and billboard rotation. |

Property: `group: THREE.Group` — parent into the scene.

### `class Toolbar`

`src/ui/Toolbar.ts`

```ts
constructor(opts: ToolbarOptions)
```

```ts
interface ToolbarOptions {
  buttons: Array<{ id: string; label: string; onSelect: () => void; active?: boolean }>;
}
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `setActive` | `(id: string) => void` | Highlight the button with this id. |
| `triggerById` | `(id: string) => void` | Programmatically fire a button's onSelect. |

Properties: `group: THREE.Group`, `buttons: ToolbarButton[]`.

### `class Breadcrumb`

`src/ui/Breadcrumb.ts`

Clickable breadcrumb trail for hierarchy drill navigation.

```ts
constructor(opts: { onNavigate: (depthIndex: number) => void })
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `setPath` | `(labels: string[]) => void` | Update the trail. `labels[0]` = root, `labels[n]` = current focus. |
| `getBlocks` | `() => Array<{ block, index }>` | Clickable blocks for Interact registration. |
| `navigate` | `(depthIndex: number) => void` | Fire the onNavigate callback. |

Property: `group: THREE.Group`.

### `class VizHud`

`src/ui/VizHud.ts`

Per-viz Back/Reset buttons. Back button is hidden when drill depth is 0.

```ts
constructor(opts: { onBack?: () => void; onReset?: () => void })
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `setDrillDepth` | `(depth: number) => void` | Show/hide Back button based on depth. |
| `getBlocks` | `() => Array<{ block, id, onSelect }>` | Clickable blocks for Interact registration. |

Property: `group: THREE.Group`.

---

## Audio

### `class SpatialHoverAudio`

`src/audio/SpatialHoverAudio.ts`

```ts
constructor(defaultCamera: THREE.Camera)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `init` | `() => Promise<void>` | Initialize AudioContext and procedural tick buffer (660 Hz, 60 ms). |
| `attach` | `(id: string, object: THREE.Object3D) => void` | Create a PositionalAudio node parented to the object. |
| `play` | `(id: string) => void` | Play the tick sound at the object's position. |
| `reparentTo` | `(obj: THREE.Object3D) => void` | Move the AudioListener to a different parent (e.g. XR camera). |

Property: `listener: THREE.AudioListener`.

### `class AmbientBed`

`src/audio/AmbientBed.ts`

Omnitone FOA/HOA wrapper for per-dataspace ambient audio beds.

```ts
constructor(ctx: AudioContext, listener: THREE.Object3D)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `load` | `(opts: AmbientBedOptions) => Promise<void>` | Fetch and decode an ambisonic audio file, initialize Omnitone renderer. |
| `loadFromBuffer` | `(buffer: AudioBuffer, opts?) => Promise<void>` | Load from a pre-decoded AudioBuffer. |
| `start` | `() => void` | Start playback (looped). |
| `stop` | `() => void` | Stop playback. |
| `update` | `() => void` | Call each frame to sync head-pose rotation matrix to Omnitone. |
| `setGain` | `(v: number) => void` | Set output gain. |

```ts
interface AmbientBedOptions {
  url: string;
  order?: 1 | 2 | 3;            // default: 1 (FOA)
  gain?: number;                 // default: 0.5
}
```

---

## Manifest system

### Types

`src/manifest/schema.ts`

```ts
interface DataspaceManifest {
  version: '1';
  name: string;
  scaleTag: 'personal' | 'room' | 'hall' | 'net';
  owner?: string;
  ambisonicBedUrl?: string;
  acousticEnvironment?: 'indoor' | 'outdoor' | 'auto';
  marks: MarkSpec[];
  joinCode?: { rotationSeconds: number; charSet: 'alphanumeric-unambiguous'; length: number };
}

interface MarkSpec {
  id: string;
  type: MarkType;
  title: string;
  subtitle?: string;
  data: InlineData | UrlData;
  config?: Record<string, unknown>;
  drillable?: boolean;
  hoverable?: boolean;
  draggable?: boolean;
}

type MarkType =
  | 'line' | 'bar' | 'scatter' | 'arc'
  | 'tree' | 'treemap' | 'sunburst' | 'pack'
  | 'force' | 'ridgeline' | 'sankey'
  | 'parallel' | 'tangled-tree' | 'edge-bundle' | 'hexbin';
```

### `loadManifest(manifest) -> Promise<LoadResult>`

`src/manifest/loader.ts`

Reads a `DataspaceManifest`, fetches any URL data sources, and instantiates marks using registered builders.

```ts
interface LoadResult {
  name: string;
  scaleTag: string;
  marks: LoadedMark[];
}

interface LoadedMark {
  id: string;
  type: MarkType;
  title: string;
  subtitle?: string;
  group: THREE.Group;
  viz: unknown;
  drillable: boolean;
  hoverable: boolean;
  draggable: boolean;
}
```

### `registerMarkBuilder(type, builder)`

Register a builder function for a mark type. Called once per type at startup.

```ts
function registerMarkBuilder(type: MarkType, builder: (spec: MarkSpec) => LoadedMark | null): void
```

### `registerAllBuilders()`

`src/manifest/builders.ts`

Registers builders for: `tree`, `treemap`, `sunburst`, `pack`, `force`, `ridgeline`, `sankey`. Call once at startup.

Helper extractors: `extractHierarchy(spec)`, `extractGraph(spec)`, `extractFlow(spec)`, `extractSeries(spec)`, `extractDistributions(spec)`.

---

## Dataspace

`src/dataspace/Dataspace.ts`

### `class DataspaceRegistry`

```ts
add(ds: Dataspace): void
list(): Dataspace[]
get(id: string): Dataspace | undefined
focus(id: string | null): void
getFocused(): string | null
onFocusChange(fn: (id: string | null) => void): void
```

```ts
interface Dataspace {
  id: string;
  name: string;
  scaleTag: 'personal' | 'room' | 'hall' | 'net';
  color: number;
  glyph: string;
}
```

### `class DataspaceHud`

```ts
constructor(registry: DataspaceRegistry)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `rebuild` | `() => void` | Recreate all chips from registry. |
| `refresh` | `() => void` | Update chip styling based on focused dataspace (called automatically on focus change). |

Property: `group: THREE.Group`.

### `applyFocusDim(group, markDataspaceId, focused)`

Dims a mark's Object3D when its dataspace is not focused: scale 0.7, opacity 0.08, emissive suppressed.

### `SCALE_RADIUS`

```ts
const SCALE_RADIUS: Record<ScaleTag, number> = {
  personal: 0.3,
  room: 1.5,
  hall: 4,
  net: 8,
};
```

---

## XR Rig

`src/xrRig.ts`

### `class XRRig`

```ts
constructor(renderer: THREE.WebGLRenderer, scene: THREE.Scene, events?: XRRigEvents)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `update` | `(frame: XRFrame \| undefined) => void` | Call each frame. Updates beams, reticles, fingertip spheres. |
| `getHandJointState` | `(i: number) => HandJointInfo \| null` | Get index-finger-tip position and pinch distance for hand `i`. |

Property: `controllers: THREE.Group[]` — the two XR controller groups.

```ts
interface XRRigEvents {
  onSelectStart?: (controllerIndex: number) => void;
  onSelectEnd?: (controllerIndex: number) => void;
}

interface HandJointInfo {
  fingertipPos: THREE.Vector3;
  pinchDistance: number;
}
```

---

## Palette

`src/ui/palette.ts`

Warm amber/cream palette tuned for optical passthrough readability.

```ts
const TEXT = {
  primary:  0xffd97a,    // warm amber — titles, headers, callouts
  body:     0xf5e9c8,    // warm cream — body copy, node labels
  muted:    0xb8a380,    // muted warm — subtitles, secondary info
  dim:      0x7a6e5a,    // dim — disabled, off-state
  emphasis: 0xffffff,    // pure white — value readouts
  accent:   0x88ff99,    // soft mint — success, active state
  warn:     0xffb873,    // warm peach — warning, attention
  error:    0xff7a8a,    // warm coral — error
} as const;
```

---

## Tween utilities

`src/util/tween.ts`

All tween functions return `{ tick(): void; done: boolean }`. Call `tick()` each frame.

```ts
function easeExpOut(t: number): number
// Exponential-out easing: fast start, gentle settle.

function tweenInstanced(
  mesh: THREE.InstancedMesh,
  from: Array<{ pos: THREE.Vector3; scale: number }>,
  to: Array<{ pos: THREE.Vector3; scale: number }>,
  duration: number,
  easing?: (t: number) => number,
): { tick(): void; done: boolean }
// Tween InstancedMesh positions with uniform scale.

function tweenInstancedBoxes(
  mesh: THREE.InstancedMesh,
  from: Array<{ pos: THREE.Vector3; scale: THREE.Vector3 }>,
  to: Array<{ pos: THREE.Vector3; scale: THREE.Vector3 }>,
  duration: number,
  easing?: (t: number) => number,
): { tick(): void; done: boolean }
// Tween InstancedMesh with per-instance 3-axis scale (treemap boxes).

function tweenMeshes(
  meshes: THREE.Mesh[],
  from: Array<{ pos: THREE.Vector3; scale: THREE.Vector3 }>,
  to: Array<{ pos: THREE.Vector3; scale: THREE.Vector3 }>,
  duration: number,
  easing?: (t: number) => number,
): { tick(): void; done: boolean }
// Tween an array of individual Mesh objects (sunburst, pack).
```

---

## `window.__demo` hooks

Testing and debugging hooks available at runtime. Used by the smoke test and useful for console interaction.

### Camera / scene

- `setCameraPose(pos: [x,y,z], look: [x,y,z])` — reposition the camera.
- `showVizGallery(show: boolean)` — toggle between gallery and chart scenes.

### Chart marks

- `hover(id: string | null)` — programmatic hover on a chart mark.
- `brush(id: string, x0: number, x1: number) -> BrushResult` — programmatic brush.
- `clearBrushes()` — clear all active brushes.
- `toggleSelect(id: string)` — toggle pin-select on a chart mark.
- `clearSelections()` — clear all selections.

### Dataspace

- `focusDataspace(id: string | null)` — focus a dataspace (dims others).

### Audio

- `startAmbient()` — start the Omnitone FOA ambient bed.
- `stopAmbient()` — stop the ambient bed.
- `audioState() -> 'off' | 'loading' | 'on' | 'error'` — current audio state.

### Force graph

- `forceHover(i: number | null)` — hover a force node.
- `forcePin(i: number, offset: [x,y,z])` — pin a node with offset.
- `forceUnpin(i: number)` — unpin a node.
- `forceHoverHand(handIndex, i)` — per-hand hover.
- `forcePinHand(handIndex, i, offset)` — per-hand pin.
- `forceUnpinHand(handIndex, i)` — per-hand unpin.
- `forceNodeCount() -> number`
- `forceWorldCenter() -> [x,y,z]`
- `lookAtForce(distance: number)` — aim camera at force graph center.
- `forceBrushSelect(indices: number[])` — simulate brush selection.

### Tree

- `treeHover(i: number | null)` — hover a tree node.
- `treeToggleSelect(i: number)` — toggle selection.
- `treeClearSelections()` — clear selections.
- `treeNodeCount() -> number`
- `treeNodeInfo(i: number) -> TreeNodeInfo`
- `treeDrillIn(i: number) -> boolean`
- `treeDrillOut() -> boolean`
- `treeFocusPath() -> number[]`
- `treeFocusLabels() -> string[]`
- `lookAtTree(distance: number)` — aim camera at tree center.

### Treemap

- `treemapHover(i)`, `treemapToggleSelect(i)`, `treemapClearSelections()`
- `treemapNodeCount()`, `treemapNodeInfo(i)`
- `treemapDrillIn(i)`, `treemapDrillOut()`
- `lookAtTreemap(distance)`

### Sunburst

- `sunburstHover(i)`, `sunburstToggleSelect(i)`, `sunburstClearSelections()`
- `sunburstSegmentCount()`, `sunburstSegmentInfo(i)`
- `sunburstDrillIn(i)`, `sunburstDrillOut()`
- `lookAtSunburst(distance)`

### Pack

- `packHover(i)`, `packToggleSelect(i)`, `packClearSelections()`
- `packNodeCount()`, `packNodeInfo(i)`
- `packDrillIn(i)`, `packDrillOut()`
- `lookAtPack(distance)`

### Sankey

- `sankeyHover(i)`
- `sankeyNodeCount()`, `sankeyNodeInfo(i)`
- `lookAtSankey(distance)`

### Ridgeline

- `ridgelineTick(t: number)` — advance animation to time `t`.

### Live data

- `updateHRData() -> { updated, dataLength }` — manually trigger an HR data update.
- `setLiveHR(enabled: boolean)` — enable/disable automatic 2s HR updates.

### Tidy tree

- `tidyTreeNodeCount() -> number`
- `lookAtTidyTree(distance: number)` — aim camera at tidy tree center.

### Tangled tree

- `tangledTreeNodeCount() -> number`
- `lookAtTangledTree(distance: number)` — aim camera at tangled tree center.

### Parallel coordinates

- `parallelNodeCount() -> number`
- `lookAtParallel(distance: number)` — aim camera at parallel coordinates center.

### Edge bundle

- `edgeBundleNodeCount() -> number`
- `lookAtEdgeBundle(distance: number)` — aim camera at edge bundle center.

### Morph demo

- `startMorph()` — enter morph mode (hides gallery, starts auto-cycle).
- `nextMorph()` — advance to the next layout in the cycle.
- `stopMorph()` — exit morph mode (returns to gallery).
- `morphCurrentType() -> string` — current layout type (`'tree'`, `'sunburst'`, `'treemap'`, `'pack'`).
- `lookAtMorph(distance: number)` — aim camera at morph demo center.

### Gallery

- `galleryItems() -> Array<{ id, title, worldPos }>` — list gallery cells.

### Join panel (M20)

- `showJoinPanel()` — show the join-code onboarding panel.
- `hideJoinPanel()` — hide the join panel.
- `fillJoinCode(code: string)` — programmatically fill the code slots (e.g. `'ABC123'`).
- `submitJoinCode()` — trigger the submit action.
