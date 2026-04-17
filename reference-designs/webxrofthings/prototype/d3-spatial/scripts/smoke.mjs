import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { setTimeout as sleep } from 'node:timers/promises';

const OUT_DIR = 'demo/shots';
const GALLERY = 'demo/index.html';
const MANIFEST = 'demo/shots.json';
mkdirSync(OUT_DIR, { recursive: true });

const SHOTS = [
  // M1.1 — mark overview and closeups
  { id: 'm11-overview', ms: 'M1.1', title: 'All marks · overview',
    caption: 'line · bar · scatter · arc',
    pos: [0, 1.3, 0.85], look: [0, 1.3, 0], hover: null },
  { id: 'm11-angle',    ms: 'M1.1', title: 'All marks · 3/4 angle',
    caption: '3D perspective confirms extruded geometry',
    pos: [0.45, 1.6, 0.8], look: [0, 1.3, 0], hover: null },
  { id: 'm11-line',     ms: 'M1.1', title: 'Closeup · line (HR ribbon)',
    caption: 'TubeGeometry along CatmullRomCurve3, d3-scaleTime on x',
    pos: [-0.18, 1.42, 0.35], look: [-0.18, 1.42, 0], hover: null },
  { id: 'm11-bar',      ms: 'M1.1', title: 'Closeup · bar (room temp)',
    caption: 'InstancedMesh of boxes, 12 samples',
    pos: [0.18, 1.42, 0.35], look: [0.18, 1.42, 0], hover: null },
  { id: 'm11-scatter',  ms: 'M1.1', title: 'Closeup · scatter (poster points)',
    caption: 'InstancedMesh of spheres, 180 instances',
    pos: [-0.18, 1.16, 0.35], look: [-0.18, 1.16, 0], hover: null },
  { id: 'm11-arc',      ms: 'M1.1', title: 'Closeup · arc (breadcrumb)',
    caption: 'TubeGeometry arc — TBOC breadcrumb primitive',
    pos: [0.18, 1.16, 0.35], look: [0.18, 1.16, 0], hover: null },

  // M2 — hover feedback
  { id: 'm2-hover-line',    ms: 'M2', title: 'Hover · line',
    caption: 'Dramatic feedback: scale 1.05, emissive bump, outline',
    pos: [0, 1.3, 0.85], look: [0, 1.3, 0], hover: 'line' },
  { id: 'm2-hover-bar',     ms: 'M2', title: 'Hover · bar',
    caption: 'Same feedback pattern across mark types',
    pos: [0, 1.3, 0.85], look: [0, 1.3, 0], hover: 'bar' },
  { id: 'm2-hover-scatter', ms: 'M2', title: 'Hover · scatter',
    caption: 'Outline visible at desktop resolution, legible through passthrough',
    pos: [0, 1.3, 0.85], look: [0, 1.3, 0], hover: 'scatter' },
  { id: 'm2-hover-arc',     ms: 'M2', title: 'Hover · arc',
    caption: 'Breadcrumb arc highlighted',
    pos: [0, 1.3, 0.85], look: [0, 1.3, 0], hover: 'arc' },

  // M3 — three-mesh-ui inspector + spatial audio
  { id: 'm3-inspector-line', ms: 'M3', title: 'Inspector · line',
    caption: 'ThreeMeshUI.Block panel with troika text, placed adjacent to hovered mark',
    pos: [-0.18, 1.42, 0.42], look: [-0.05, 1.42, 0], hover: 'line' },
  { id: 'm3-inspector-bar',  ms: 'M3', title: 'Inspector · bar',
    caption: 'Same panel template, different content per mark',
    pos: [0.18, 1.42, 0.42], look: [0.3, 1.42, 0], hover: 'bar' },
  { id: 'm3-inspector-scatter', ms: 'M3', title: 'Inspector · scatter',
    caption: 'Inspector reads UC3 poster dataspace manifest values',
    pos: [-0.18, 1.16, 0.42], look: [-0.05, 1.16, 0], hover: 'scatter' },
  { id: 'm3-inspector-arc',  ms: 'M3', title: 'Inspector · arc + breadcrumb',
    caption: 'TBOC breadcrumb arc with chapter trail readout',
    pos: [0.18, 1.16, 0.42], look: [0.3, 1.16, 0], hover: 'arc' },
  { id: 'm3-overview',       ms: 'M3', title: 'M3 overview · inspector floating near line',
    caption: 'Shows spatial relationship: chart left, inspector right. Spatial audio: PositionalAudio at each mark (not audible in screenshot).',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], hover: 'line' },

  // M3.1 — smarter placement (right mark -> inspector to the LEFT, not overlapping)
  { id: 'm31-placement-bar', ms: 'M3.1', title: 'Inspector placement · bar (auto flip left)',
    caption: 'placeNear auto-selects side with clearance — bar is on the right half, card flips to the left',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], hover: 'bar' },
  { id: 'm31-placement-line', ms: 'M3.1', title: 'Inspector placement · line (auto right)',
    caption: 'line is on the left half, card auto-places to the right',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], hover: 'line' },

  // M4 — brush selection
  { id: 'm4-brush-line', ms: 'M4', title: 'Brush · line (middle 40%)',
    caption: 'Translucent cyan selection rectangle + count readout in inspector',
    pos: [-0.18, 1.42, 0.42], look: [-0.05, 1.42, 0], hover: null, brush: ['line', 0.3, 0.7] },
  { id: 'm4-brush-bar', ms: 'M4', title: 'Brush · bar (last 30%)',
    caption: 'Same brush primitive works across mark types',
    pos: [0.18, 1.42, 0.42], look: [0.3, 1.42, 0], hover: null, brush: ['bar', 0.7, 1.0] },
  { id: 'm4-brush-scatter', ms: 'M4', title: 'Brush · scatter (left half)',
    caption: 'Point-in-range filter over InstancedMesh',
    pos: [-0.18, 1.16, 0.42], look: [-0.05, 1.16, 0], hover: null, brush: ['scatter', 0, 0.5] },
  { id: 'm4-brush-overview', ms: 'M4', title: 'Brush overview · two marks selected',
    caption: 'Inspector shows last-brushed count; chart retains selection',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], hover: null, brush: ['line', 0.2, 0.6] },

  { id: 'm4a-select-bar', ms: 'M4.1', title: 'Trigger = activate · bar selected',
    caption: 'Trigger toggles a latched "selected" state. Outline + inspector pin even when you look away. Trigger again to release.',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], hover: null, select: 'bar' },
];

// M5 — real pointer-drag brush (screenshots orchestrated by page.mouse)
// M6 — ambient audio (Omnitone FOA drone, procedural buffer)
// M7 — dataspace federation focus
const POINTER_SHOTS = [
  { id: 'm5-drag-start', ms: 'M5', title: 'Drag brush · start',
    caption: 'Real pointer down on line chart; brush rectangle appears',
    pos: [0, 1.3, 0.85], look: [0, 1.3, 0],
    mouse: { down: [360, 350], move: null } },
  { id: 'm5-drag-mid', ms: 'M5', title: 'Drag brush · mid-drag',
    caption: 'Rubberband widens as pointer moves right',
    pos: [0, 1.3, 0.85], look: [0, 1.3, 0],
    mouse: { down: [360, 350], move: [470, 350], up: false } },
  { id: 'm5-drag-end', ms: 'M5', title: 'Drag brush · released',
    caption: 'Selection persists; inspector shows sample count',
    pos: [0, 1.3, 0.85], look: [0, 1.3, 0],
    mouse: { down: [360, 350], move: [500, 350], up: true } },
];

const FEATURE_SHOTS = [
  { id: 'm6-ambient-on', ms: 'M6', title: 'Ambient bed · ON',
    caption: 'Omnitone FOA renderer initialized with a procedural 4-channel buffer. Rotation matrix updated per frame.',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], action: 'ambient-on' },
  { id: 'm6-ambient-off', ms: 'M6', title: 'Ambient bed · off',
    caption: 'Baseline state — bed not started; user must opt in',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], action: 'ambient-off' },

  { id: 'm7-focus-uc1', ms: 'M7', title: 'Dataspace focus · UC1 (wrist)',
    caption: 'line mark stays bright; room + poster marks dim (0.25 opacity, 0.85 scale)',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], action: 'focus-UC1' },
  { id: 'm7-focus-uc2', ms: 'M7', title: 'Dataspace focus · UC2 (room)',
    caption: 'bar mark active; wrist and poster dim',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], action: 'focus-UC2' },
  { id: 'm7-focus-uc3', ms: 'M7', title: 'Dataspace focus · UC3 (poster)',
    caption: 'scatter + arc both belong to UC3 and stay bright together',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], action: 'focus-UC3' },
  { id: 'm7-focus-all', ms: 'M7', title: 'Dataspace focus · all (null)',
    caption: 'Clearing focus returns every mark to full brightness',
    pos: [0, 1.3, 0.95], look: [0, 1.3, 0], action: 'focus-null' },
];

// M8–M13 — §9 spatial hierarchy and graph marks, in the viz gallery
const GALLERY_SHOTS = [
  { id: 'm8-gallery-overview', ms: 'M8', title: 'Viz gallery · overview',
    caption: '6 new marks from §9: tree (radial), treemap (extruded), sunburst (stacked), circular packing (spheres), force (3D physics), ridgeline (depth-offset)',
    pos: [0, 1.3, 1.4], look: [0, 1.3, 0] },
  { id: 'm8-gallery-angle', ms: 'M8', title: 'Viz gallery · 3/4 angle',
    caption: 'Perspective confirms every mark has real z-depth',
    pos: [0.7, 1.6, 1.1], look: [0, 1.3, 0] },

  { id: 'm9-tree-radial', ms: 'M9', title: 'Tree · radial (§9.1)',
    caption: 'd3-cluster layout remapped to polar; InstancedMesh spheres + LineSegments edges',
    pos: [-0.42, 1.47, 0.45], look: [-0.42, 1.47, 0] },
  { id: 'm9-treemap', ms: 'M9', title: 'Treemap · extruded (§9.2)',
    caption: 'd3-treemap layout; extrusion height = value (second quantity)',
    pos: [0, 1.47, 0.45], look: [0, 1.47, 0] },
  { id: 'm9-sunburst', ms: 'M9', title: 'Sunburst · stacked discs (§9.5)',
    caption: 'd3-partition with polar remap; one disc per hierarchy level, +z per level',
    pos: [0.42, 1.47, 0.45], look: [0.42, 1.47, 0] },

  { id: 'm10-pack', ms: 'M10', title: 'Circular packing · nested spheres (§9.9 ★)',
    caption: 'd3-pack layout; volume encodes value, nesting encodes hierarchy',
    pos: [-0.42, 1.13, 0.45], look: [-0.42, 1.13, 0] },
  { id: 'm10-force', ms: 'M10', title: 'Force-directed graph · 3D physics (§9.6)',
    caption: 'd3-force-3d simulation settled for 200 ticks; nodes and links in full 3D',
    pos: [0, 1.13, 0.45], look: [0, 1.13, 0] },
  { id: 'm10-ridgeline', ms: 'M10', title: 'Ridgeline · depth-offset (§9.9 ★)',
    caption: '6 density rows offset in z instead of y — a mountain range in front of the reader',
    pos: [0.42, 1.13, 0.45], look: [0.42, 1.13, 0] },

  { id: 'm11-gallery-top', ms: 'M11', title: 'Viz gallery · top-down',
    caption: 'All six marks from above; treemap and packing read as city footprints',
    pos: [0, 2.0, 0.05], look: [0, 1.3, 0] },
  { id: 'm11-gallery-wide', ms: 'M11', title: 'Viz gallery · wide context',
    caption: 'Reader at ~2m; full gallery fits in frame with surrounding grid',
    pos: [0, 1.6, 1.9], look: [0, 1.3, 0] },
];

const FORCE_SHOTS = [
  { id: 'm12-force-pre', ms: 'M12', title: 'Force graph · before interaction',
    caption: 'Settled baseline before any node is touched',
    distance: 0.22, action: 'none' },
  { id: 'm12-force-hover', ms: 'M12', title: 'Force graph · hover on node 3',
    caption: 'Shared halo + billboarded label appear at hovered node; physics keeps running',
    distance: 0.22, action: 'hover', nodeId: 3 },
  { id: 'm12-force-hover2', ms: 'M12', title: 'Force graph · hover on node 14',
    caption: 'Same halo + label primitive re-parents to a different node',
    distance: 0.22, action: 'hover', nodeId: 14 },
  { id: 'm12-force-drag-up', ms: 'M12', title: 'Force graph · drag node up',
    caption: 'Node pinned with fx/fy/fz; surrounding graph relaxes via physics',
    distance: 0.22, action: 'drag', nodeId: 5, offset: [0, 0.08, 0] },
  { id: 'm12-force-drag-out', ms: 'M12', title: 'Force graph · drag node toward reader',
    caption: 'Pull a node out of the cluster; edges tension visibly',
    distance: 0.22, action: 'drag', nodeId: 12, offset: [0.05, 0.02, 0.08] },
  { id: 'm12-force-after', ms: 'M12', title: 'Force graph · after release (physics relaxed)',
    caption: 'Unpin; the graph settles into a new low-energy layout',
    distance: 0.22, action: 'settle' },
];

const TREE_SHOTS = [
  { id: 'm13a-tree-pre', ms: 'M13a', title: 'Tree · radial baseline',
    caption: 'Per-node hover + pinch-select now wired, same pattern as the force graph',
    distance: 0.28, action: 'none' },
  { id: 'm13a-tree-hover-leaf', ms: 'M13a', title: 'Tree · hover on a leaf',
    caption: 'Halo + billboarded label over the hovered node (leaf "gateway")',
    distance: 0.28, action: 'hover', nodeIdx: 9 },
  { id: 'm13a-tree-hover-internal', ms: 'M13a', title: 'Tree · hover on an internal node',
    caption: 'Same infrastructure works for non-leaves ("compute")',
    distance: 0.28, action: 'hover', nodeIdx: 8 },
  { id: 'm13a-tree-select-one', ms: 'M13a', title: 'Tree · one node selected',
    caption: 'Pinch latches a mint-green selection halo; persists when you look away',
    distance: 0.28, action: 'select', nodeIdx: 5 },
  { id: 'm13a-tree-select-many', ms: 'M13a', title: 'Tree · multiple nodes selected',
    caption: 'Multi-select — each pinch toggles independently',
    distance: 0.28, action: 'select-many', nodeIdxs: [3, 5, 7, 10, 14] },
];

// M13b — treemap, sunburst, pack per-node hover + pinch-select
const TREEMAP_SHOTS = [
  { id: 'm13b-treemap-pre', ms: 'M13b', title: 'Treemap · baseline',
    caption: 'Extruded treemap with per-cell hover + pinch-select now wired',
    distance: 0.28, action: 'none' },
  { id: 'm13b-treemap-hover', ms: 'M13b', title: 'Treemap · hover on cell 2',
    caption: 'Shared halo + billboarded label at hovered treemap cell',
    distance: 0.28, action: 'hover', nodeIdx: 2 },
  { id: 'm13b-treemap-select', ms: 'M13b', title: 'Treemap · cell selected',
    caption: 'Mint-green selection halo on treemap cell',
    distance: 0.28, action: 'select', nodeIdx: 4 },
  { id: 'm13b-treemap-select-many', ms: 'M13b', title: 'Treemap · multiple cells selected',
    caption: 'Multi-select across treemap cells',
    distance: 0.28, action: 'select-many', nodeIdxs: [0, 3, 6, 9] },
];

const SUNBURST_SHOTS = [
  { id: 'm13b-sunburst-pre', ms: 'M13b', title: 'Sunburst · baseline',
    caption: 'Stacked sunburst with per-segment hover + pinch-select',
    distance: 0.28, action: 'none' },
  { id: 'm13b-sunburst-hover', ms: 'M13b', title: 'Sunburst · hover on segment 3',
    caption: 'Shared halo + label at hovered sunburst segment centroid',
    distance: 0.28, action: 'hover', segIdx: 3 },
  { id: 'm13b-sunburst-select', ms: 'M13b', title: 'Sunburst · segment selected',
    caption: 'Selection marker on sunburst segment',
    distance: 0.28, action: 'select', segIdx: 5 },
];

const PACK_SHOTS = [
  { id: 'm13b-pack-pre', ms: 'M13b', title: 'Circular packing · baseline',
    caption: 'Nested spheres with per-node hover + pinch-select',
    distance: 0.28, action: 'none' },
  { id: 'm13b-pack-hover', ms: 'M13b', title: 'Pack · hover on node 4',
    caption: 'Halo + label on hovered pack node',
    distance: 0.28, action: 'hover', nodeIdx: 4 },
  { id: 'm13b-pack-select', ms: 'M13b', title: 'Pack · node selected',
    caption: 'Mint-green selection halo around packed sphere',
    distance: 0.28, action: 'select', nodeIdx: 6 },
];

// M14 — drill-in transitions
const DRILL_SHOTS = [
  { id: 'm14-tree-drill-pre', ms: 'M14', title: 'Tree · before drill-in',
    caption: 'Full tree baseline before drilling into a subtree',
    viz: 'tree', distance: 0.28, action: 'none' },
  { id: 'm14-tree-drill-in', ms: 'M14', title: 'Tree · drilled into "sensors" subtree',
    caption: 'Tree re-lays-out with the sensors node as root, animated transition',
    viz: 'tree', distance: 0.28, action: 'drill-in', nodeIdx: 1 },
  { id: 'm14-tree-drill-out', ms: 'M14', title: 'Tree · drilled back out',
    caption: 'Tree returns to full hierarchy after drillOut',
    viz: 'tree', distance: 0.28, action: 'drill-out' },
  { id: 'm14-treemap-drill-pre', ms: 'M14', title: 'Treemap · before drill-in',
    caption: 'Full treemap baseline',
    viz: 'treemap', distance: 0.28, action: 'none' },
  { id: 'm14-treemap-drill-in', ms: 'M14', title: 'Treemap · drilled into subtree',
    caption: 'Treemap re-lays-out with a parent node as root',
    viz: 'treemap', distance: 0.28, action: 'drill-in', nodeIdx: 0 },
  { id: 'm14-treemap-drill-out', ms: 'M14', title: 'Treemap · drilled back out',
    caption: 'Treemap returns to full layout',
    viz: 'treemap', distance: 0.28, action: 'drill-out' },
  { id: 'm14-sunburst-drill-in', ms: 'M14', title: 'Sunburst · drilled into subtree',
    caption: 'Sunburst re-partitions with focused subtree',
    viz: 'sunburst', distance: 0.28, action: 'drill-in', segIdx: 0 },
  { id: 'm14-sunburst-drill-out', ms: 'M14', title: 'Sunburst · drilled back out',
    caption: 'Sunburst returns to full partition',
    viz: 'sunburst', distance: 0.28, action: 'drill-out' },
  { id: 'm14-pack-drill-in', ms: 'M14', title: 'Pack · drilled into subtree',
    caption: 'Pack re-layouts with focused subtree, animated transition',
    viz: 'pack', distance: 0.28, action: 'drill-in', nodeIdx: 1 },
  { id: 'm14-pack-drill-out', ms: 'M14', title: 'Pack · drilled back out',
    caption: 'Pack returns to full hierarchy',
    viz: 'pack', distance: 0.28, action: 'drill-out' },
];

const server = spawn('npx', ['vite', '--port', '5179', '--host', '127.0.0.1'], {
  stdio: ['ignore', 'pipe', 'pipe'],
  env: { ...process.env, NO_COLOR: '1' },
});
let serverReady = false;
server.stdout.on('data', d => { if (d.toString().includes('Local:')) serverReady = true; });
server.stderr.on('data', d => process.stderr.write(`[vite:err] ${d}`));

const t0 = Date.now();
while (!serverReady && Date.now() - t0 < 20000) await sleep(200);
if (!serverReady) { console.error('vite did not start'); server.kill(); process.exit(1); }
await sleep(500);

const browser = await chromium.launch({
  args: [
    '--use-gl=angle', '--use-angle=swiftshader', '--enable-webgl', '--ignore-gpu-blocklist',
    '--disable-background-timer-throttling',
    '--disable-backgrounding-occluded-windows',
    '--disable-renderer-backgrounding',
    '--autoplay-policy=no-user-gesture-required',
  ],
});
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });

const errors = [];
page.on('pageerror', e => errors.push(`pageerror: ${e.message}`));
page.on('console', msg => {
  const t = msg.type();
  if (t === 'error') errors.push(`console.error: ${msg.text()}`);
  else if (msg.text().includes('[hardClear]') || msg.text().includes('[fb.off]') || msg.text().includes('[interact]')) console.log('[browser]', msg.text());
});

const shots = [];
try {
  await page.goto('http://127.0.0.1:5179/', { waitUntil: 'load', timeout: 15000 });
  await page.waitForFunction(() => (window).__demo?.setCameraPose, { timeout: 10000 });
  await sleep(1500);
  await page.evaluate(() => (window).__demo.showVizGallery(false));
  await sleep(300);

  for (const s of SHOTS) {
    await page.evaluate(({ pos, look, hover, brush, select }) => {
      const d = (window).__demo;
      d.setCameraPose(pos, look);
      d.clearBrushes?.();
      d.clearSelections?.();
      d.focusDataspace?.(null);
      if (brush) {
        d.hover(null);
        d.brush(brush[0], brush[1], brush[2]);
      } else if (select) {
        d.hover(null);
        d.toggleSelect(select);
      } else {
        d.hover(hover);
      }
    }, s);
    await sleep(400);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }

  // M5 — real pointer-drag brush via page.mouse
  for (const s of POINTER_SHOTS) {
    await page.evaluate(({ pos, look }) => {
      const d = (window).__demo;
      d.setCameraPose(pos, look);
      d.clearBrushes();
      d.hover(null);
      d.focusDataspace(null);
    }, s);
    await sleep(250);
    const [dx, dy] = s.mouse.down;
    await page.mouse.move(dx, dy);
    await page.mouse.down();
    if (s.mouse.move) {
      const [mx, my] = s.mouse.move;
      await page.mouse.move(mx, my, { steps: 8 });
    }
    await sleep(150);
    if (s.mouse.up) await page.mouse.up();
    await sleep(200);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    if (!s.mouse.up) await page.mouse.up();
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }

  // park mouse off canvas so Interact won't hover during feature shots
  await page.mouse.move(5, 5);

  // Hide gallery for the older milestone shots
  await page.evaluate(() => (window).__demo.showVizGallery(false));

  // M6 + M7 — ambient audio and dataspace focus
  for (const s of FEATURE_SHOTS) {
    await page.evaluate(async ({ pos, look, action }) => {
      const d = (window).__demo;
      d.setCameraPose(pos, look);
      d.clearBrushes();
      d.hover(null);
      d.focusDataspace(null);
      if (action === 'ambient-on') await d.startAmbient();
      if (action === 'ambient-off') d.stopAmbient();
      if (action?.startsWith('focus-')) {
        const id = action.replace('focus-', '');
        d.focusDataspace(id === 'null' ? null : id);
      }
    }, s);
    await sleep(400);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }

  // M8+ — §9 viz gallery
  await page.evaluate(() => {
    const d = (window).__demo;
    d.hover(null);
    d.clearBrushes();
    d.focusDataspace(null);
    d.stopAmbient();
    d.showVizGallery(true);
  });
  await sleep(500);

  for (const s of GALLERY_SHOTS) {
    await page.evaluate(({ pos, look }) => {
      (window).__demo.setCameraPose(pos, look);
    }, s);
    await sleep(350);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }

  // M12 — force graph interaction (hover, drag, release)
  let pinnedDuringSession = null;
  for (const s of FORCE_SHOTS) {
    await page.evaluate(({ distance, action, nodeId, offset }) => {
      const d = (window).__demo;
      d.lookAtForce(distance);
      d.forceHover(null);
      if (action === 'none') return;
      if (action === 'hover') d.forceHover(nodeId);
      if (action === 'drag') { d.forcePin(nodeId, offset); d.forceHover(nodeId); }
      if (action === 'settle') {
        if (window.__lastPinned != null) d.forceUnpin(window.__lastPinned);
      }
      if (action === 'drag') window.__lastPinned = nodeId;
    }, s);
    await sleep(s.action === 'drag' ? 700 : (s.action === 'settle' ? 1400 : 350));
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
    pinnedDuringSession = s.action === 'drag' ? s.nodeId : pinnedDuringSession;
  }
  await page.evaluate(() => { (window).__demo.forceHover(null); });

  // M13a — tree per-node hover + pinch-select
  for (const s of TREE_SHOTS) {
    await page.evaluate(({ distance, action, nodeIdx, nodeIdxs }) => {
      const d = (window).__demo;
      d.lookAtTree(distance);
      d.treeHover(null);
      d.treeClearSelections();
      if (action === 'none') return;
      if (action === 'hover') d.treeHover(nodeIdx);
      if (action === 'select') { d.treeToggleSelect(nodeIdx); d.treeHover(nodeIdx); }
      if (action === 'select-many') {
        for (const i of nodeIdxs) d.treeToggleSelect(i);
        d.treeHover(null);
      }
    }, s);
    await sleep(350);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }
  await page.evaluate(() => {
    const d = (window).__demo;
    d.treeHover(null);
    d.treeClearSelections();
  });

  // M13b — treemap per-cell hover + select
  for (const s of TREEMAP_SHOTS) {
    await page.evaluate(({ distance, action, nodeIdx, nodeIdxs }) => {
      const d = (window).__demo;
      d.lookAtTreemap(distance);
      d.treemapHover(null);
      d.treemapClearSelections();
      if (action === 'none') return;
      if (action === 'hover') d.treemapHover(nodeIdx);
      if (action === 'select') { d.treemapToggleSelect(nodeIdx); d.treemapHover(nodeIdx); }
      if (action === 'select-many') {
        for (const i of nodeIdxs) d.treemapToggleSelect(i);
        d.treemapHover(null);
      }
    }, s);
    await sleep(350);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }
  await page.evaluate(() => { (window).__demo.treemapHover(null); (window).__demo.treemapClearSelections(); });

  // M13b — sunburst per-segment hover + select
  for (const s of SUNBURST_SHOTS) {
    await page.evaluate(({ distance, action, segIdx }) => {
      const d = (window).__demo;
      d.lookAtSunburst(distance);
      d.sunburstHover(null);
      d.sunburstClearSelections();
      if (action === 'none') return;
      if (action === 'hover') d.sunburstHover(segIdx);
      if (action === 'select') { d.sunburstToggleSelect(segIdx); d.sunburstHover(segIdx); }
    }, s);
    await sleep(350);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }
  await page.evaluate(() => { (window).__demo.sunburstHover(null); (window).__demo.sunburstClearSelections(); });

  // M13b — pack per-node hover + select
  for (const s of PACK_SHOTS) {
    await page.evaluate(({ distance, action, nodeIdx }) => {
      const d = (window).__demo;
      d.lookAtPack(distance);
      d.packHover(null);
      d.packClearSelections();
      if (action === 'none') return;
      if (action === 'hover') d.packHover(nodeIdx);
      if (action === 'select') { d.packToggleSelect(nodeIdx); d.packHover(nodeIdx); }
    }, s);
    await sleep(350);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }
  await page.evaluate(() => { (window).__demo.packHover(null); (window).__demo.packClearSelections(); });

  // M14 — drill-in transitions
  for (const s of DRILL_SHOTS) {
    await page.evaluate(({ viz, distance, action, nodeIdx, segIdx }) => {
      const d = (window).__demo;
      const lookFn = viz === 'tree' ? d.lookAtTree : viz === 'treemap' ? d.lookAtTreemap : viz === 'sunburst' ? d.lookAtSunburst : d.lookAtPack;
      lookFn.call(d, distance);
      if (action === 'none') return;
      if (action === 'drill-in') {
        if (viz === 'tree') d.treeDrillIn(nodeIdx);
        else if (viz === 'treemap') d.treemapDrillIn(nodeIdx);
        else if (viz === 'sunburst') d.sunburstDrillIn(segIdx);
        else if (viz === 'pack') d.packDrillIn(nodeIdx);
      }
      if (action === 'drill-out') {
        if (viz === 'tree') d.treeDrillOut();
        else if (viz === 'treemap') d.treemapDrillOut();
        else if (viz === 'sunburst') d.sunburstDrillOut();
        else if (viz === 'pack') d.packDrillOut();
      }
    }, s);
    // Wait for tween to complete
    await sleep(s.action === 'none' ? 350 : 700);
    const file = `${OUT_DIR}/${s.id}.png`;
    await page.screenshot({ path: file, fullPage: false });
    shots.push({
      id: s.id, milestone: s.ms, title: s.title, caption: s.caption,
      file: `shots/${s.id}.png`, ts: new Date().toISOString(),
    });
    console.log(`  shot -> ${file}`);
  }
} catch (e) {
  console.error('capture failed:', e.message);
  errors.push(`capture: ${e.message}`);
}

await browser.close();
server.kill();

if (errors.length) {
  console.error('\nSMOKE FAILED:');
  for (const e of errors) console.error('  -', e);
  process.exit(1);
}

writeFileSync(MANIFEST, JSON.stringify({ generatedAt: new Date().toISOString(), shots }, null, 2));
writeFileSync(GALLERY, renderGallery(shots));
console.log(`\nSMOKE OK · gallery -> ${GALLERY}`);

function renderGallery(shots) {
  const byMs = {};
  for (const s of shots) {
    if (!byMs[s.milestone]) byMs[s.milestone] = [];
    byMs[s.milestone].push(s);
  }
  const sections = Object.entries(byMs).map(([ms, items]) => `
    <section>
      <h2>${ms}</h2>
      <div class="grid">
        ${items.map(s => `
          <figure class="card">
            <a href="${s.file}" target="_blank"><img src="${s.file}" alt="${s.title}" /></a>
            <figcaption>
              <strong>${s.title}</strong>
              <span class="cap">${s.caption}</span>
              <time>${s.ts}</time>
            </figcaption>
          </figure>
        `).join('')}
      </div>
    </section>
  `).join('');

  return `<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8" />
<title>d3-spatial · progress gallery</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; padding: 24px; background: #0b1220; color: #cde; font-family: ui-sans-serif, system-ui, sans-serif; }
  header { margin-bottom: 24px; }
  h1 { margin: 0 0 4px 0; font-size: 22px; color: #8cf; }
  h2 { margin: 28px 0 12px 0; font-size: 16px; color: #9ab; border-bottom: 1px solid #1d2b44; padding-bottom: 6px; letter-spacing: 0.04em; }
  header p { margin: 0; color: #89a; font-size: 13px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(380px, 1fr)); gap: 20px; }
  .card { margin: 0; background: #0f1a2c; border: 1px solid #1d2b44; border-radius: 8px; overflow: hidden; }
  .card img { display: block; width: 100%; height: auto; }
  figcaption { padding: 10px 14px; display: flex; flex-direction: column; gap: 2px; font-size: 13px; }
  figcaption strong { color: #8cf; }
  .cap { color: #9ab; }
  time { color: #556677; font-size: 11px; font-family: ui-monospace, monospace; }
  footer { margin-top: 32px; color: #556677; font-size: 12px; }
</style></head>
<body>
<header>
  <h1>d3-spatial · progress gallery</h1>
  <p>Auto-generated by <code>npm run smoke</code>. Grouped by milestone. Click any thumbnail for full-res.</p>
</header>
<main>${sections}</main>
<footer>Generated ${new Date().toISOString()}</footer>
</body></html>`;
}
