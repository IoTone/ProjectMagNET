import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { buildTree, TreeViz } from '../viz/tree';
import { buildTreemap, TreemapViz } from '../viz/treemap';
import { buildSunburst, SunburstViz } from '../viz/sunburst';
import { buildCircularPack, PackViz } from '../viz/pack';
import { buildForceGraph, ForceViz } from '../viz/force';
import { buildRidgeline, RidgelineViz } from '../viz/ridgeline';
import { buildSankey, SankeyViz } from '../viz/sankey';
import { buildStreamgraph, StreamgraphViz } from '../viz/streamgraph';
import { buildTidyTree, TidyTreeViz } from '../viz/tidyTree';
import { buildTangledTree, TangledTreeViz } from '../viz/tangledTree';
import { buildParallel, ParallelViz } from '../viz/parallel';
import { buildEdgeBundle, EdgeBundleViz } from '../viz/edgeBundle';
import { buildMorphDemo, MorphDemo } from './morphDemo';
import { buildVideoPanel, VideoPanelViz } from '../viz/videoPanel';
import { buildLiveLineCell, buildLivePhasesCell, buildLiveTargetsCell, LiveCell } from './liveVitalsCells';
import { sampleTree, sampleGraph, sampleRidgeline, sampleSankey, sampleTangles, sampleParallel, sampleStreamgraph } from './sampleHierarchy';
import { TEXT } from '../ui/palette';

export interface GalleryItem {
  id: string;
  title: string;
  group: THREE.Group;
  worldPos: THREE.Vector3;
}

export interface GalleryResult {
  root: THREE.Group;
  items: GalleryItem[];
  force: ForceViz;
  forceCell: THREE.Group;
  tree: TreeViz;
  treeCell: THREE.Group;
  treemap: TreemapViz;
  treemapCell: THREE.Group;
  sunburst: SunburstViz;
  sunburstCell: THREE.Group;
  pack: PackViz;
  packCell: THREE.Group;
  ridgeline: RidgelineViz;
  sankey: SankeyViz;
  sankeyCell: THREE.Group;
  streamgraph: StreamgraphViz;
  streamgraphCell: THREE.Group;
  tidyTree: TidyTreeViz;
  tidyTreeCell: THREE.Group;
  tangledTree: TangledTreeViz;
  tangledTreeCell: THREE.Group;
  parallel: ParallelViz;
  parallelCell: THREE.Group;
  edgeBundle: EdgeBundleViz;
  edgeBundleCell: THREE.Group;
  morphDemo: MorphDemo;
  morphCell: THREE.Group;
  videoPanel: VideoPanelViz;
  videoCell: THREE.Group;
  liveHr: LiveCell;
  liveHrCell: THREE.Group;
  liveBr: LiveCell;
  liveBrCell: THREE.Group;
  livePhases: LiveCell;
  livePhasesCell: THREE.Group;
  liveTargets: LiveCell;
  liveTargetsCell: THREE.Group;
}

export function buildVizGallery(): GalleryResult {
  const root = new THREE.Group();
  root.name = 'viz-gallery';

  const items: GalleryItem[] = [];
  const force = buildForceGraph(sampleGraph(28));
  const tree = buildTree(sampleTree, { form: 'radial' });
  const treemap = buildTreemap(sampleTree);
  const sunburst = buildSunburst(sampleTree);
  const pack = buildCircularPack(sampleTree);
  const ridgeline = buildRidgeline(sampleRidgeline(6));
  const sankey = buildSankey(sampleSankey());
  const streamgraph = buildStreamgraph(sampleStreamgraph(), { width: 0.32, height: 0.16, windowSize: 60, scrollSpeed: 6 });
  const tidyTree = buildTidyTree(sampleTree);
  const tangledTree = buildTangledTree(sampleTree, sampleTangles());
  const parallel = buildParallel(sampleParallel());
  const edgeBundle = buildEdgeBundle(sampleTree, sampleGraph(28));
  const morphDemo = buildMorphDemo(sampleTree);
  // Camera URL. Default: /camera/capture (served via Vite proxy). This keeps
  // the camera on the LAN and only tunnels the app — cloudflared never sees
  // the camera directly, avoiding header-buffer overruns and connection resets.
  const CAMERA_URL = import.meta.env.VITE_CAMERA_URL ?? '/camera/capture';
  const CAMERA_MODE = (import.meta.env.VITE_CAMERA_MODE as 'mjpeg' | 'frames') ?? 'frames';
  const videoPanel = buildVideoPanel({
    url: CAMERA_URL,
    type: CAMERA_MODE,
    width: 0.28,
    aspectRatio: 4 / 3,
    title: 'video · ESP32-CAM',
    frameIntervalMs: 1000,
  });

  // Live MagNET Vitals device — pulled through the Vite proxy (`/api/v1/vitals/*`).
  // HR/BR use snapshot endpoints (`/heart-rate`, `/breathing`) and accumulate
  // into a client-side rolling buffer — the device's `/history` endpoints
  // only update every 60 s (firmware ring-buffer cadence), which makes the
  // line look frozen in a demo. 3 s × 60 buffer = 3-minute trace; cadence is
  // a deliberate compromise between visible motion and not overloading the
  // device's esp_http_server (4 cells × ~0.5 req/s each was crashing the C6).
  const liveHr = buildLiveLineCell({
    url: '/api/v1/vitals/heart-rate',
    refreshMs: 3000, width: 0.32, height: 0.16,
    color: 0xff7a8a, vMin: 40, vMax: 130,
    pluck: (j) => (j?.presence && typeof j.bpm === 'number' && j.bpm > 0) ? j.bpm : null,
    historyLength: 60,
  });
  const liveBr = buildLiveLineCell({
    url: '/api/v1/vitals/breathing',
    refreshMs: 3000, width: 0.32, height: 0.16,
    color: 0xffb873, vMin: 4, vMax: 30,
    pluck: (j) => (typeof j?.rpm === 'number' && j.rpm > 0) ? j.rpm : null,
    historyLength: 60,
  });
  const livePhases = buildLivePhasesCell({
    url: '/api/v1/vitals/phases',
    refreshMs: 1500, width: 0.32, height: 0.16,
    windowSize: 120, scrollSpeed: 12,
  });
  // Tabletop tilt (~70°) lays the floor map mostly horizontal so the orbs
  // read as physical bodies in space rather than dots on a wall poster.
  const liveTargets = buildLiveTargetsCell({
    url: '/api/v1/vitals/targets',
    refreshMs: 1000, width: 0.32, height: 0.16,
    extent_m: 2.5, max_distance_m: 4.0, fov_deg: 50,
    tilt_rad: (70 * Math.PI) / 180,
    glyph_radius: 0.014,
    glyph_lift: 0.03,
  });

  const specs = [
    { id: 'tree',       title: 'tree \u00b7 radial',              viz: tree.group,          sublabel: '\u00a79.1 hierarchy \u00b7 node-link' },
    { id: 'treemap',    title: 'treemap \u00b7 extruded',         viz: treemap.group,       sublabel: '\u00a79.2 hierarchy \u00b7 area + z' },
    { id: 'sunburst',   title: 'sunburst \u00b7 stacked',         viz: sunburst.group,      sublabel: '\u00a79.5 hierarchy \u00b7 radial partition' },
    { id: 'pack',       title: 'circular packing',           viz: pack.group,          sublabel: '\u00a79.9 \u2605 nested spheres' },
    { id: 'force',      title: 'force \u00b7 d3-force-3d',        viz: force.group,         sublabel: '\u00a79.6 graph \u00b7 3D physics' },
    { id: 'ridgeline',  title: 'ridgeline \u00b7 depth-offset',   viz: ridgeline.group,     sublabel: '\u00a79.9 \u2605 distribution' },
    { id: 'sankey',     title: 'sankey \u00b7 3D tubes',          viz: sankey.group,        sublabel: '\u00a79.9 \u2605 flow network' },
    { id: 'streamgraph',title: 'streamgraph \u00b7 animated',     viz: streamgraph.group,   sublabel: 'stacked timeseries \u00b7 wiggle' },
    { id: 'tidyTree',   title: 'tidy tree \u00b7 cylindrical',    viz: tidyTree.group,      sublabel: '\u00a79 Reingold-Tilford \u00b7 cylinder' },
    { id: 'tangledTree',title: 'tangled tree \u00b7 arcs',        viz: tangledTree.group,   sublabel: '\u00a79 tree + cross-links' },
    { id: 'parallel',   title: 'parallel coords',            viz: parallel.group,      sublabel: '\u00a79 multivariate axes' },
    { id: 'edgeBundle', title: 'edge bundling',               viz: edgeBundle.group,    sublabel: '\u00a79 hierarchical routing' },
    { id: 'morph',      title: 'morph \u00b7 layout transition',  viz: morphDemo.group,     sublabel: 'tree \u2192 sunburst \u2192 treemap \u2192 pack' },
    { id: 'video',      title: 'video \u00b7 ESP32-CAM',          viz: videoPanel.group,    sublabel: 'live MJPEG camera feed' },
    { id: 'liveHr',     title: 'HR \u00b7 live',                  viz: liveHr.group,        sublabel: 'MagNET Vitals \u00b7 60 min, 1/min' },
    { id: 'liveBr',     title: 'BR \u00b7 live',                  viz: liveBr.group,        sublabel: 'MagNET Vitals \u00b7 60 min, 1/min' },
    { id: 'livePhases', title: 'phases \u00b7 live',              viz: livePhases.group,    sublabel: 'mmWave heart/breath/total \u00b7 ~10 Hz' },
    { id: 'liveTargets',title: 'targets \u00b7 live',             viz: liveTargets.group,   sublabel: 'mmWave floor map \u00b7 top-down \u00b7 1 Hz' },
  ];

  const cols = 4;
  const rows = Math.ceil(specs.length / cols);
  const cellW = 0.38;
  const cellH = 0.30;
  const rowGap = 0.10;

  specs.forEach((s, i) => {
    const col = i % cols;
    const row = Math.floor(i / cols);
    const x = (col - (cols - 1) / 2) * cellW;
    const y = ((rows - 1) / 2 - row) * (cellH + rowGap);

    const cell = new THREE.Group();
    cell.position.set(x, y, 0);
    cell.add(s.viz);

    const title = new Text();
    title.text = s.title;
    title.fontSize = 0.018;
    title.color = TEXT.primary;
    title.anchorX = 'center';
    title.anchorY = 'bottom';
    title.position.set(0, cellH / 2 + 0.015, 0.03);
    title.sync();
    cell.add(title);

    const sub = new Text();
    sub.text = s.sublabel;
    sub.fontSize = 0.011;
    sub.color = TEXT.muted;
    sub.anchorX = 'center';
    sub.anchorY = 'top';
    sub.position.set(0, -cellH / 2 - 0.015, 0.03);
    sub.sync();
    cell.add(sub);

    root.add(cell);
    items.push({ id: s.id, title: s.title, group: cell, worldPos: new THREE.Vector3(x, y, 0) });
  });

  const forceCell = items.find(i => i.id === 'force')!.group;
  const treeCell = items.find(i => i.id === 'tree')!.group;
  const treemapCell = items.find(i => i.id === 'treemap')!.group;
  const sunburstCell = items.find(i => i.id === 'sunburst')!.group;
  const packCell = items.find(i => i.id === 'pack')!.group;
  const sankeyCell = items.find(i => i.id === 'sankey')!.group;
  const streamgraphCell = items.find(i => i.id === 'streamgraph')!.group;
  const tidyTreeCell = items.find(i => i.id === 'tidyTree')!.group;
  const tangledTreeCell = items.find(i => i.id === 'tangledTree')!.group;
  const parallelCell = items.find(i => i.id === 'parallel')!.group;
  const edgeBundleCell = items.find(i => i.id === 'edgeBundle')!.group;
  const morphCell = items.find(i => i.id === 'morph')!.group;
  const videoCell = items.find(i => i.id === 'video')!.group;
  const liveHrCell = items.find(i => i.id === 'liveHr')!.group;
  const liveBrCell = items.find(i => i.id === 'liveBr')!.group;
  const livePhasesCell = items.find(i => i.id === 'livePhases')!.group;
  const liveTargetsCell = items.find(i => i.id === 'liveTargets')!.group;

  return {
    root, items, force, forceCell, tree, treeCell, treemap, treemapCell,
    sunburst, sunburstCell, pack, packCell, ridgeline, sankey, sankeyCell,
    streamgraph, streamgraphCell,
    tidyTree, tidyTreeCell, tangledTree, tangledTreeCell,
    parallel, parallelCell, edgeBundle, edgeBundleCell,
    morphDemo, morphCell,
    videoPanel, videoCell,
    liveHr, liveHrCell, liveBr, liveBrCell,
    livePhases, livePhasesCell, liveTargets, liveTargetsCell,
  };
}
