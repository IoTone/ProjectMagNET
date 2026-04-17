import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { ARButton } from 'three/examples/jsm/webxr/ARButton.js';
import { XRRig } from './xrRig';
import { Text } from 'troika-three-text';
import { buildDemoScene } from './demo/marks';
import { buildVizGallery } from './demo/vizGallery';
import { Interact, hoverFeedback } from './interact/Interact';
import { InspectorCard } from './ui/InspectorCard';
import { Toolbar } from './ui/Toolbar';
import { TEXT } from './ui/palette';
import { NodeHoverFx } from './ui/NodeHoverFx';
import { Breadcrumb } from './ui/Breadcrumb';
import { VizHud } from './ui/VizHud';
import { SpatialHoverAudio } from './audio/SpatialHoverAudio';
import { AmbientBed } from './audio/AmbientBed';
import { makeAmbientFoaBuffer } from './audio/proceduralBed';
import { DragBrush } from './interact/DragBrush';
import { DataspaceRegistry, DataspaceHud, applyFocusDim } from './dataspace/Dataspace';

const renderer = new THREE.WebGLRenderer({
  antialias: true,
  alpha: true,
  preserveDrawingBuffer: true,
});
renderer.setPixelRatio(window.devicePixelRatio);
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.xr.enabled = true;
renderer.setClearColor(0x0b1220, 1);

renderer.xr.addEventListener('sessionstart', () => {
  renderer.setClearColor(0x000000, 0);
});
renderer.xr.addEventListener('sessionend', () => {
  renderer.setClearColor(0x0b1220, 1);
});
document.body.appendChild(renderer.domElement);

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(60, window.innerWidth / window.innerHeight, 0.01, 100);
camera.position.set(0, 1.4, 1.2);
camera.lookAt(0, 1.2, 0);

scene.add(new THREE.HemisphereLight(0xffffff, 0x444466, 0.9));
const dir = new THREE.DirectionalLight(0xffffff, 0.6);
dir.position.set(1, 2, 1);
scene.add(dir);

const floorGrid = new THREE.GridHelper(4, 16, 0x224466, 0x112233);
(floorGrid.material as THREE.Material).transparent = true;
(floorGrid.material as THREE.Material).opacity = 0.35;
floorGrid.userData.noHover = true;
scene.add(floorGrid);

const EYE_TO_FLOOR_M = 1.55;

function placeFloorUnderHead() {
  const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
  const pos = new THREE.Vector3();
  xrCam.getWorldPosition(pos);
  floorGrid.position.set(pos.x, pos.y - EYE_TO_FLOOR_M, pos.z);
  console.log('[floor] placed at y=', (pos.y - EYE_TO_FLOOR_M).toFixed(2), 'cam y=', pos.y.toFixed(2));
}

placeFloorUnderHead();

const uiAnchor = new THREE.Group();
uiAnchor.name = 'uiAnchor';
scene.add(uiAnchor);

const label = new Text();
label.text = 'd3-spatial · M3 — panels, inspector, spatial audio';
label.fontSize = 0.028;
label.color = TEXT.primary;
label.anchorX = 'center';
label.anchorY = 'middle';
label.position.set(0, 0.33, 0);
label.sync();
uiAnchor.add(label);

const { root: demoRoot, marks: demoMarks } = buildDemoScene();
uiAnchor.add(demoRoot);

const vizAnchor = new THREE.Group();
vizAnchor.name = 'vizAnchor';
scene.add(vizAnchor);
const { root: galleryRoot, items: galleryItems, force: forceViz, tree: treeViz, treemap: treemapViz, sunburst: sunburstViz, pack: packViz, ridgeline: ridgelineViz, sankey: sankeyViz, treeCell, treemapCell, sunburstCell, packCell } = buildVizGallery();
vizAnchor.add(galleryRoot);

// Per-hand NodeHoverFx: indices 0, 1 = XR hands; 2 = mouse/desktop
const nodeHoverFxs: NodeHoverFx[] = [];
for (let i = 0; i < 3; i++) {
  const fx = new NodeHoverFx(camera);
  scene.add(fx.group);
  nodeHoverFxs.push(fx);
}
// Convenience alias for desktop / legacy paths
const nodeHoverFx: NodeHoverFx = nodeHoverFxs[2]!;
vizAnchor.position.set(0, 1.3, 0);
const urlScene = new URLSearchParams(window.location.search).get('scene');
const defaultToGallery = urlScene !== 'charts';
vizAnchor.visible = defaultToGallery;
uiAnchor.visible = !defaultToGallery;

function showVizGallery(show: boolean) {
  vizAnchor.visible = show;
  uiAnchor.visible = !show;
  if (toolbar) toolbar.setActive(show ? 'gallery' : 'charts');
  if (show) {
    const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
    const pos = new THREE.Vector3();
    xrCam.getWorldPosition(pos);
    const fwd = new THREE.Vector3(0, 0, -1).applyQuaternion(xrCam.quaternion);
    fwd.y = 0;
    if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
    fwd.normalize();
    vizAnchor.position.set(pos.x + fwd.x * 1.4, pos.y - 0.05, pos.z + fwd.z * 1.4);
    vizAnchor.lookAt(pos.x, vizAnchor.position.y, pos.z);
  }
}

const registry = new DataspaceRegistry();
registry.add({ id: 'UC1', name: 'wrist',   scaleTag: 'personal', color: 0xff5577, glyph: '👤' });
registry.add({ id: 'UC2', name: 'room',    scaleTag: 'room',     color: 0x66ccff, glyph: '🏠' });
registry.add({ id: 'UC3', name: 'poster',  scaleTag: 'hall',     color: 0xffcc66, glyph: '🏛' });

const markDataspace: Record<string, string> = {
  line: 'UC1', bar: 'UC2', scatter: 'UC3', arc: 'UC3',
};

const dsHud = new DataspaceHud(registry);
dsHud.group.position.set(0, 0.24, 0);
uiAnchor.add(dsHud.group);

registry.onFocusChange(focused => {
  for (const m of demoMarks) applyFocusDim(m.group, markDataspace[m.id]!, focused);
});

function hideInspector() { inspector.hide(); }

function hardClearAllHoverFeedback() {
  selectedMarks.clear();
  for (const [, fb] of feedbacks) fb.off();
}

const rig = new XRRig(renderer, scene, {
  onSelectStart: (controllerIndex: number) => {
    if (!interact.beginDragForHand(controllerIndex)) interact.setPressLockedForHand(controllerIndex, true);
  },
  onSelectEnd: (controllerIndex: number) => {
    if (interact.isDraggingForHand(controllerIndex)) { interact.endDragForHand(controllerIndex); return; }
    interact.triggerSelectForHand(controllerIndex);
    interact.setPressLockedForHand(controllerIndex, false);
  },
});

const interact = new Interact(camera, renderer.domElement, renderer);
interact.setXrControllers(rig.controllers);
const inspector = new InspectorCard();
uiAnchor.add(inspector.block);

// Per-hand force drag state: indices 0, 1 = XR hands; 2 = mouse
const forceDragNodeIds: (number | null)[] = [null, null, null];
const tmpForceVecs: THREE.Vector3[] = [new THREE.Vector3(), new THREE.Vector3(), new THREE.Vector3()];
const tmpForceVec = tmpForceVecs[2]!; // legacy alias

/** Safe hand-index accessor — always returns a valid NodeHoverFx. */
function fxForHand(hi: number): NodeHoverFx { return nodeHoverFxs[hi] ?? nodeHoverFxs[2]!; }
function vecForHand(hi: number): THREE.Vector3 { return tmpForceVecs[hi] ?? tmpForceVecs[2]!; }

interact.add({
  id: 'force:nodes',
  object: forceViz.nodeMesh,
  supportsInstances: true,
  onHoverIn: (ctx) => {
    const hi = ctx?.handIndex ?? 2;
    if (ctx?.instanceId !== undefined) {
      forceViz.getNodeWorldPosition(ctx.instanceId, vecForHand(hi));
      fxForHand(hi).show(vecForHand(hi), forceViz.getNodeLabel(ctx.instanceId));
    }
  },
  onHoverOut: (ctx) => {
    const hi = ctx?.handIndex ?? 2;
    fxForHand(hi).hide();
  },
  onHoverInstance: (instanceId, handIndex) => {
    const hi = handIndex ?? 2;
    if (instanceId === null) { fxForHand(hi).hide(); return; }
    forceViz.getNodeWorldPosition(instanceId, vecForHand(hi));
    fxForHand(hi).show(vecForHand(hi), forceViz.getNodeLabel(instanceId));
  },
  onDragStart: (ctx) => {
    if (ctx.instanceId === undefined) return false;
    const hi = ctx.handIndex ?? 2;
    forceDragNodeIds[hi] = ctx.instanceId;
    return true;
  },
  onDragMove: (worldPoint, handIndex) => {
    const hi = handIndex ?? 2;
    const nodeId = forceDragNodeIds[hi];
    if (nodeId !== null && nodeId !== undefined) {
      forceViz.pinNode(nodeId, worldPoint);
      fxForHand(hi).updatePosition(worldPoint);
    }
  },
  onDragEnd: (ctx) => {
    const hi = ctx?.handIndex ?? 2;
    const nodeId = forceDragNodeIds[hi];
    if (nodeId !== null && nodeId !== undefined) forceViz.unpinNode(nodeId);
    forceDragNodeIds[hi] = null;
  },
});

const tmpTreeVec = new THREE.Vector3();
interact.add({
  id: 'tree:nodes',
  object: treeViz.nodeMesh,
  supportsInstances: true,
  onHoverIn: (ctx) => {
    if (ctx?.instanceId !== undefined) {
      treeViz.getNodeWorldPosition(ctx.instanceId, tmpTreeVec);
      fxForHand(ctx?.handIndex ?? 2).show(tmpTreeVec, treeViz.getNodeLabel(ctx.instanceId));
    }
  },
  onHoverOut: (ctx) => { fxForHand(ctx?.handIndex ?? 2).hide(); },
  onHoverInstance: (instanceId, handIndex) => {
    const fx = fxForHand(handIndex ?? 2);
    if (instanceId === null) { fx.hide(); return; }
    treeViz.getNodeWorldPosition(instanceId, tmpTreeVec);
    fx.show(tmpTreeVec, treeViz.getNodeLabel(instanceId));
  },
  onSelect: (ctx) => {
    if (ctx?.instanceId === undefined) return;
    const info = treeViz.getNodeInfo(ctx.instanceId);
    if (!info.isLeaf && info.childCount > 0) {
      treeViz.drillIn(ctx.instanceId);
      if (treeEntry) updateBreadcrumbAndHud(treeEntry);
    } else {
      treeViz.toggleSelected(ctx.instanceId);
    }
  },
});

// --- Treemap interaction (InstancedMesh) ---
const tmpTreemapVec = new THREE.Vector3();
interact.add({
  id: 'treemap:cells',
  object: treemapViz.nodeMesh,
  supportsInstances: true,
  onHoverIn: (ctx) => {
    if (ctx?.instanceId !== undefined) {
      treemapViz.getNodeWorldPosition(ctx.instanceId, tmpTreemapVec);
      fxForHand(ctx?.handIndex ?? 2).show(tmpTreemapVec, treemapViz.getNodeLabel(ctx.instanceId));
    }
  },
  onHoverOut: (ctx) => { fxForHand(ctx?.handIndex ?? 2).hide(); },
  onHoverInstance: (instanceId, handIndex) => {
    const fx = fxForHand(handIndex ?? 2);
    if (instanceId === null) { fx.hide(); return; }
    treemapViz.getNodeWorldPosition(instanceId, tmpTreemapVec);
    fx.show(tmpTreemapVec, treemapViz.getNodeLabel(instanceId));
  },
  onSelect: (ctx) => {
    if (ctx?.instanceId === undefined) return;
    if (treemapViz.drillIn(ctx.instanceId)) {
      if (treemapEntry) updateBreadcrumbAndHud(treemapEntry);
    } else {
      treemapViz.toggleSelected(ctx.instanceId);
    }
  },
});

// --- Sunburst interaction (individual meshes, group-level hover) ---
const tmpSunburstVec = new THREE.Vector3();
interact.add({
  id: 'sunburst:segments',
  object: sunburstViz.group,
  onHoverIn: (ctx) => {
    const segIdx = ctx?.hitObject?.userData?.segmentIndex;
    if (segIdx !== undefined) {
      sunburstViz.getSegmentWorldPosition(segIdx, tmpSunburstVec);
      fxForHand(ctx?.handIndex ?? 2).show(tmpSunburstVec, sunburstViz.getSegmentLabel(segIdx));
    }
  },
  onHoverOut: (ctx) => { fxForHand(ctx?.handIndex ?? 2).hide(); },
  onSelect: (ctx) => {
    const segIdx = ctx?.hitObject?.userData?.segmentIndex;
    if (segIdx === undefined) return;
    const info = sunburstViz.getSegmentInfo(segIdx);
    if (!info.isLeaf && info.childCount > 0) {
      sunburstViz.drillIn(segIdx);
      if (sunburstEntry) updateBreadcrumbAndHud(sunburstEntry);
    } else {
      sunburstViz.toggleSelected(segIdx);
    }
  },
});

// --- Pack interaction (individual meshes, group-level hover) ---
const tmpPackVec = new THREE.Vector3();
interact.add({
  id: 'pack:nodes',
  object: packViz.group,
  onHoverIn: (ctx) => {
    const nodeIdx = ctx?.hitObject?.userData?.nodeIndex;
    if (nodeIdx !== undefined) {
      packViz.getNodeWorldPosition(nodeIdx, tmpPackVec);
      fxForHand(ctx?.handIndex ?? 2).show(tmpPackVec, packViz.getNodeLabel(nodeIdx));
    }
  },
  onHoverOut: (ctx) => { fxForHand(ctx?.handIndex ?? 2).hide(); },
  onSelect: (ctx) => {
    const nodeIdx = ctx?.hitObject?.userData?.nodeIndex;
    if (nodeIdx === undefined) return;
    const info = packViz.getNodeInfo(nodeIdx);
    if (!info.isLeaf && info.childCount > 0) {
      packViz.drillIn(nodeIdx);
      if (packEntry) updateBreadcrumbAndHud(packEntry);
    } else {
      packViz.toggleSelected(nodeIdx);
    }
  },
});

// --- Sankey interaction (InstancedMesh) ---
const tmpSankeyVec = new THREE.Vector3();
interact.add({
  id: 'sankey:nodes',
  object: sankeyViz.nodeMesh,
  supportsInstances: true,
  onHoverIn: (ctx) => {
    if (ctx?.instanceId !== undefined) {
      sankeyViz.getNodeWorldPosition(ctx.instanceId, tmpSankeyVec);
      fxForHand(ctx?.handIndex ?? 2).show(tmpSankeyVec, sankeyViz.getNodeLabel(ctx.instanceId));
    }
  },
  onHoverOut: (ctx) => { fxForHand(ctx?.handIndex ?? 2).hide(); },
  onHoverInstance: (instanceId, handIndex) => {
    const fx = fxForHand(handIndex ?? 2);
    if (instanceId === null) { fx.hide(); return; }
    sankeyViz.getNodeWorldPosition(instanceId, tmpSankeyVec);
    fx.show(tmpSankeyVec, sankeyViz.getNodeLabel(instanceId));
  },
  onSelect: (ctx) => {
    if (ctx?.instanceId === undefined) return;
    // No drill-in for sankey; just show info via hover
  },
});

// --- M16: Breadcrumb trails + VizHud per-cell buttons for hierarchy marks ---
const CELL_H = 0.32;

interface HierarchyVizEntry {
  id: string;
  viz: { drillOut(): boolean; getFocusPath(): number[]; getFocusLabels(): string[]; clearSelection(): void };
  cell: THREE.Group;
  breadcrumb: Breadcrumb;
  hud: VizHud;
}

const hierarchyVizEntries: HierarchyVizEntry[] = [];

function setupHierarchyUI(
  id: string,
  viz: HierarchyVizEntry['viz'],
  cell: THREE.Group,
) {
  // Breadcrumb: above cell title
  const breadcrumb = new Breadcrumb({
    onNavigate: (depthIndex: number) => {
      const path = viz.getFocusPath();
      // Drill out to target depth: depthIndex = 0 means root, etc.
      const targetDepth = depthIndex;
      while (viz.getFocusPath().length > targetDepth) {
        viz.drillOut();
      }
      updateBreadcrumbAndHud(entry);
    },
  });
  breadcrumb.group.position.set(0, CELL_H / 2 + 0.005, 0.001);
  cell.add(breadcrumb.group);

  // VizHud: below cell sublabel
  const hud = new VizHud({
    onBack: () => {
      viz.drillOut();
      updateBreadcrumbAndHud(entry);
    },
    onReset: () => {
      while (viz.getFocusPath().length > 0) {
        viz.drillOut();
      }
      viz.clearSelection();
      updateBreadcrumbAndHud(entry);
    },
  });
  hud.group.position.set(0, -CELL_H / 2 - 0.02, 0.001);
  cell.add(hud.group);

  const entry: HierarchyVizEntry = { id, viz, cell, breadcrumb, hud };
  hierarchyVizEntries.push(entry);

  // Register breadcrumb blocks with Interact
  for (const { block, index } of breadcrumb.getBlocks()) {
    interact.add({
      id: `bc:${id}:${index}`,
      object: block,
      onHoverIn: () => {
        (block as any).set({ backgroundColor: new THREE.Color(0x2a4a6a), backgroundOpacity: 1.0 });
      },
      onHoverOut: () => {
        const labels = viz.getFocusLabels();
        const isLast = index === labels.length - 1;
        (block as any).set({
          backgroundColor: new THREE.Color(isLast ? 0x2a3a1a : 0x0f1a2c),
          backgroundOpacity: 0.88,
        });
      },
      onSelect: () => {
        breadcrumb.navigate(index);
      },
    });
  }

  // Register VizHud buttons with Interact
  for (const { block, id: btnId, onSelect } of hud.getBlocks()) {
    interact.add({
      id: `hud:${id}:${btnId}`,
      object: block,
      onHoverIn: () => {
        (block as any).set({ backgroundColor: new THREE.Color(0x2a4a6a), backgroundOpacity: 1.0 });
      },
      onHoverOut: () => {
        const origBg = btnId === 'back' ? 0x2a1a0f : 0x0f1a2c;
        (block as any).set({ backgroundColor: new THREE.Color(origBg), backgroundOpacity: 0.9 });
      },
      onSelect: () => onSelect(),
    });
  }

  return entry;
}

function updateBreadcrumbAndHud(entry: HierarchyVizEntry) {
  const labels = entry.viz.getFocusLabels();
  const depth = entry.viz.getFocusPath().length;

  // Remove old interact registrations for breadcrumb blocks
  for (const { index } of entry.breadcrumb.getBlocks()) {
    interact.remove(`bc:${entry.id}:${index}`);
  }

  entry.breadcrumb.setPath(labels);
  entry.hud.setDrillDepth(depth);

  // Re-register new breadcrumb blocks
  for (const { block, index } of entry.breadcrumb.getBlocks()) {
    interact.add({
      id: `bc:${entry.id}:${index}`,
      object: block,
      onHoverIn: () => {
        (block as any).set({ backgroundColor: new THREE.Color(0x2a4a6a), backgroundOpacity: 1.0 });
      },
      onHoverOut: () => {
        const currentLabels = entry.viz.getFocusLabels();
        const isLast = index === currentLabels.length - 1;
        (block as any).set({
          backgroundColor: new THREE.Color(isLast ? 0x2a3a1a : 0x0f1a2c),
          backgroundOpacity: 0.88,
        });
      },
      onSelect: () => {
        entry.breadcrumb.navigate(index);
      },
    });
  }
}

const treeEntry = setupHierarchyUI('tree', treeViz, treeCell);
const treemapEntry = setupHierarchyUI('treemap', treemapViz, treemapCell);
const sunburstEntry = setupHierarchyUI('sunburst', sunburstViz, sunburstCell);
const packEntry = setupHierarchyUI('pack', packViz, packCell);

const audio = new SpatialHoverAudio(camera);
let ambientBed: AmbientBed | null = null;
let ambientState: 'off' | 'loading' | 'on' | 'error' = 'off';

const initAudioOnce = () => { audio.init(); };
window.addEventListener('pointerdown', initAudioOnce, { once: true });
window.addEventListener('keydown', initAudioOnce, { once: true });

async function startAmbient() {
  if (ambientState !== 'off' && ambientState !== 'error') return;
  ambientState = 'loading';
  updateAudioHud();
  try {
    await audio.init();
    const ctx = audio.listener.context as AudioContext;
    if (ctx.state === 'suspended') await ctx.resume();
    const buf = makeAmbientFoaBuffer(ctx, 4);
    ambientBed = new AmbientBed(ctx, camera);
    await ambientBed.loadFromBuffer(buf, { order: 1, gain: 0.35 });
    ambientBed.start();
    ambientState = 'on';
  } catch (e) {
    console.error('ambient bed failed:', e);
    ambientState = 'error';
  }
  updateAudioHud();
}

function stopAmbient() {
  if (!ambientBed) return;
  ambientBed.stop();
  ambientBed = null;
  ambientState = 'off';
  updateAudioHud();
}

const audioHud = new Text();
audioHud.text = '♪ ambient: off';
audioHud.fontSize = 0.014;
audioHud.color = TEXT.muted;
audioHud.anchorX = 'left';
audioHud.anchorY = 'bottom';
audioHud.position.set(-0.3, -0.28, 0);
audioHud.sync();
uiAnchor.add(audioHud);

const debugHud = new Text();
debugHud.text = '—';
debugHud.fontSize = 0.013;
debugHud.color = TEXT.muted;
debugHud.anchorX = 'right';
debugHud.anchorY = 'bottom';
debugHud.position.set(0.3, -0.28, 0);
debugHud.sync();
uiAnchor.add(debugHud);

let debugTickAcc = 0;
function updateDebugHud(dt: number) {
  debugTickAcc += dt;
  if (debugTickAcc < 0.2) return;
  debugTickAcc = 0;
  const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
  const pos = new THREE.Vector3();
  xrCam.getWorldPosition(pos);
  const xr = renderer.xr.isPresenting ? 'XR' : 'desktop';
  const hov = interact.getHoveredId() ?? '—';
  const ctrlVis = rig.controllers.filter(c => c.visible).length;
  debugHud.text = `${xr} · cam y=${pos.y.toFixed(2)} · ctrls=${ctrlVis} · hover=${hov}`;
  debugHud.sync();
}

function updateAudioHud() {
  const badges = {
    off:     { text: '♪ ambient: off', color: TEXT.dim },
    loading: { text: '♪ ambient: loading…', color: TEXT.warn },
    on:      { text: '♪ ambient: ON · 4ch FOA · HRTF rotating', color: TEXT.accent },
    error:   { text: '♪ ambient: error', color: TEXT.error },
  };
  const b = badges[ambientState];
  audioHud.text = b.text;
  audioHud.color = b.color;
  audioHud.sync();
}

const feedbacks = new Map<string, ReturnType<typeof hoverFeedback>>();
const hoverContent: Record<string, { title: string; subtitle: string; value: string }> = {
  line:    { title: 'line · HR ribbon',    subtitle: 'UC1 · personal',   value: '72 bpm · last 60 min' },
  bar:     { title: 'bar · room temp',      subtitle: 'UC2 · room',       value: '21.4 °C · 12h' },
  scatter: { title: 'scatter · points',     subtitle: 'UC3 · poster',     value: '180 samples' },
  arc:     { title: 'arc · breadcrumb',     subtitle: 'UC3 · TBOC trail', value: '3 chapters visited' },
};

const selectedMarks = new Set<string>();

function pinMark(id: string) {
  const m = demoMarks.find(x => x.id === id);
  if (!m) return;
  selectedMarks.add(id);
  feedbacks.get(id)?.on();
  inspector.show({
    title: (hoverContent[id]?.title ?? id) + ' · selected',
    subtitle: hoverContent[id]?.subtitle ?? '',
    value: hoverContent[id]?.value ?? '',
  });
  inspector.placeNear(m.group, { preferredSide: 'auto' });
}

function unpinMark(id: string) {
  selectedMarks.delete(id);
  feedbacks.get(id)?.off();
  if (interact.getHoveredId() !== id) inspector.hide();
}

for (const m of demoMarks) {
  const fb = hoverFeedback(m.group);
  feedbacks.set(m.id, fb);
  audio.attach(m.id, m.group);
  interact.add({
    id: m.id,
    object: m.group,
    onHoverIn: () => {
      fb.on();
      const content = hoverContent[m.id] ?? { title: m.id, subtitle: '', value: '' };
      const title = selectedMarks.has(m.id) ? `${content.title} · selected` : content.title;
      inspector.show({ title, subtitle: content.subtitle, value: content.value });
      inspector.placeNear(m.group);
      audio.play(m.id);
    },
    onHoverOut: (_ctx) => {
      if (selectedMarks.has(m.id)) return;
      fb.off();
      inspector.hide();
    },
    onSelect: () => {
      if (selectedMarks.has(m.id)) unpinMark(m.id);
      else pinMark(m.id);
    },
  });
}

const arButton = ARButton.createButton(renderer, {
  requiredFeatures: ['local-floor'],
  optionalFeatures: ['hand-tracking'],
});
arButton.classList.add('xr-btn');
document.body.appendChild(arButton);

let anchorPlaced = false;

renderer.xr.addEventListener('sessionstart', () => {
  anchorPlaced = false;
  setTimeout(() => {
    placeFloorUnderHead();
    placeAnchorInFrontOfUser();
    placeToolbarNearUser();
    anchorPlaced = true;
  }, 500);
});

renderer.xr.addEventListener('sessionend', () => {
  anchorPlaced = false;
});

function placeAnchorInFrontOfUser() {
  const xrCam = renderer.xr.getCamera();
  const pos = new THREE.Vector3();
  xrCam.getWorldPosition(pos);
  const fwd = new THREE.Vector3(0, 0, -1).applyQuaternion(xrCam.quaternion);
  fwd.y = 0;
  if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
  fwd.normalize();
  const active = vizAnchor.visible ? vizAnchor : uiAnchor;
  const distance = vizAnchor.visible ? 1.4 : 1.2;
  const target = new THREE.Vector3(
    pos.x + fwd.x * distance,
    pos.y - (vizAnchor.visible ? 0.05 : 0.1),
    pos.z + fwd.z * distance,
  );
  active.position.copy(target);
  active.lookAt(pos.x, active.position.y, pos.z);
  anchorPlaced = true;
  console.log('[anchor] placed at', target.toArray(), 'cam y=', pos.y.toFixed(2), 'gallery=', vizAnchor.visible);
}

window.addEventListener('resize', () => {
  camera.aspect = window.innerWidth / window.innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
});

const dragBrush = new DragBrush(camera, renderer.domElement, () => demoMarks.map(m => m.chart), {
  onStart: () => { interact.hoverById(null); },
  onUpdate: (chart, res) => {
    const mark = demoMarks.find(m => m.chart === chart);
    if (!mark) return;
    inspector.show(summarizeBrush(mark.id, res.count));
    inspector.placeNear(mark.group, { preferredSide: 'auto' });
  },
});

renderer.domElement.addEventListener('pointerdown', (e) => {
  if (e.button !== 0) return;
  if (vizAnchor.visible) interact.beginDrag();
});
window.addEventListener('pointerup', () => {
  if (interact.isDragging()) interact.endDrag();
});

function summarizeBrush(id: string, count: number): { title: string; subtitle: string; value: string } {
  const base = hoverContent[id] ?? { title: id, subtitle: '', value: '' };
  return { title: base.title, subtitle: `brushed · ${count} samples`, value: base.value };
}

(window as any).__demo = {
  camera,
  scene,
  renderer,
  uiAnchor,
  marks: demoMarks.map(m => m.id),
  setCameraPose(pos: [number, number, number], look: [number, number, number]) {
    camera.position.set(pos[0], pos[1], pos[2]);
    camera.lookAt(look[0], look[1], look[2]);
    camera.updateMatrixWorld(true);
  },
  hover(id: string | null) {
    interact.hoverById(id);
    if (id === null) { hideInspector(); hardClearAllHoverFeedback(); }
  },
  brush(id: string, x0: number, x1: number) {
    for (const m of demoMarks) m.chart.clearBrush();
    const target = demoMarks.find(m => m.id === id);
    if (!target) return null;
    const res = target.chart.brush(x0, x1);
    inspector.show(summarizeBrush(id, res.count));
    inspector.placeNear(target.group, { preferredSide: 'auto' });
    return res;
  },
  clearBrushes() {
    for (const m of demoMarks) m.chart.clearBrush();
  },
  toggleSelect(id: string) {
    if (selectedMarks.has(id)) unpinMark(id);
    else pinMark(id);
  },
  clearSelections() {
    for (const id of [...selectedMarks]) unpinMark(id);
  },
  focusDataspace(id: string | null) {
    interact.hoverById(null);
    hardClearAllHoverFeedback();
    registry.focus(id);
    hideInspector();
    for (const m of demoMarks) m.chart.clearBrush();
  },
  async startAmbient() { await startAmbient(); },
  stopAmbient() { stopAmbient(); },
  audioState: () => ambientState,
  showVizGallery(show: boolean) { showVizGallery(show); },
  galleryItems: () => galleryItems.map(i => ({ id: i.id, title: i.title, worldPos: i.worldPos.toArray() })),
  forceHover(i: number | null) {
    if (i === null) { nodeHoverFx.hide(); return; }
    forceViz.getNodeWorldPosition(i, tmpForceVec);
    nodeHoverFx.show(tmpForceVec, forceViz.getNodeLabel(i));
  },
  forcePin(i: number, offset: [number, number, number]) {
    forceViz.getNodeWorldPosition(i, tmpForceVec);
    tmpForceVec.add(new THREE.Vector3(offset[0], offset[1], offset[2]));
    forceViz.pinNode(i, tmpForceVec);
    nodeHoverFx.updatePosition(tmpForceVec);
    forceViz.reheat(0.5);
  },
  forceUnpin(i: number) { forceViz.unpinNode(i); },
  // M15 — per-hand hover/pin for two-handed force interaction
  forceHoverHand(handIndex: number, i: number | null) {
    const fx = fxForHand(handIndex);
    if (i === null) { fx.hide(); return; }
    const v = vecForHand(handIndex);
    forceViz.getNodeWorldPosition(i, v);
    fx.show(v, forceViz.getNodeLabel(i));
  },
  forcePinHand(handIndex: number, i: number, offset: [number, number, number]) {
    const v = vecForHand(handIndex);
    const fx = fxForHand(handIndex);
    forceViz.getNodeWorldPosition(i, v);
    v.add(new THREE.Vector3(offset[0], offset[1], offset[2]));
    forceViz.pinNode(i, v);
    fx.show(v, forceViz.getNodeLabel(i));
    forceViz.reheat(0.5);
  },
  forceUnpinHand(handIndex: number, i: number) {
    forceViz.unpinNode(i);
    fxForHand(handIndex).hide();
  },
  forceNodeCount: () => forceViz.nodeCount(),
  forceWorldCenter: () => {
    const v = new THREE.Vector3();
    forceViz.group.getWorldPosition(v);
    return v.toArray();
  },
  lookAtForce(distance: number) {
    const v = new THREE.Vector3();
    forceViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },
  treeHover(i: number | null) {
    if (i === null) { nodeHoverFx.hide(); return; }
    treeViz.getNodeWorldPosition(i, tmpTreeVec);
    nodeHoverFx.show(tmpTreeVec, treeViz.getNodeLabel(i));
  },
  treeToggleSelect(i: number) { return treeViz.toggleSelected(i); },
  treeClearSelections() { treeViz.clearSelection(); },
  treeNodeCount: () => treeViz.nodeCount(),
  treeNodeInfo: (i: number) => treeViz.getNodeInfo(i),
  lookAtTree(distance: number) {
    const v = new THREE.Vector3();
    treeViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },
  treeDrillIn(i: number) { const r = treeViz.drillIn(i); updateBreadcrumbAndHud(treeEntry); return r; },
  treeDrillOut() { const r = treeViz.drillOut(); updateBreadcrumbAndHud(treeEntry); return r; },
  treeFocusPath() { return treeViz.getFocusPath(); },
  treeFocusLabels() { return treeViz.getFocusLabels(); },

  // --- Treemap ---
  treemapHover(i: number | null) {
    if (i === null) { nodeHoverFx.hide(); return; }
    treemapViz.getNodeWorldPosition(i, tmpTreemapVec);
    nodeHoverFx.show(tmpTreemapVec, treemapViz.getNodeLabel(i));
  },
  treemapToggleSelect(i: number) { return treemapViz.toggleSelected(i); },
  treemapClearSelections() { treemapViz.clearSelection(); },
  treemapNodeCount: () => treemapViz.nodeCount(),
  treemapNodeInfo: (i: number) => treemapViz.getNodeInfo(i),
  treemapDrillIn(i: number) { const r = treemapViz.drillIn(i); updateBreadcrumbAndHud(treemapEntry); return r; },
  treemapDrillOut() { const r = treemapViz.drillOut(); updateBreadcrumbAndHud(treemapEntry); return r; },
  lookAtTreemap(distance: number) {
    const v = new THREE.Vector3();
    treemapViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Sunburst ---
  sunburstHover(i: number | null) {
    if (i === null) { nodeHoverFx.hide(); return; }
    sunburstViz.getSegmentWorldPosition(i, tmpSunburstVec);
    nodeHoverFx.show(tmpSunburstVec, sunburstViz.getSegmentLabel(i));
  },
  sunburstToggleSelect(i: number) { return sunburstViz.toggleSelected(i); },
  sunburstClearSelections() { sunburstViz.clearSelection(); },
  sunburstSegmentCount: () => sunburstViz.segmentCount(),
  sunburstSegmentInfo: (i: number) => sunburstViz.getSegmentInfo(i),
  sunburstDrillIn(i: number) { const r = sunburstViz.drillIn(i); updateBreadcrumbAndHud(sunburstEntry); return r; },
  sunburstDrillOut() { const r = sunburstViz.drillOut(); updateBreadcrumbAndHud(sunburstEntry); return r; },
  lookAtSunburst(distance: number) {
    const v = new THREE.Vector3();
    sunburstViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Pack ---
  packHover(i: number | null) {
    if (i === null) { nodeHoverFx.hide(); return; }
    packViz.getNodeWorldPosition(i, tmpPackVec);
    nodeHoverFx.show(tmpPackVec, packViz.getNodeLabel(i));
  },
  packToggleSelect(i: number) { return packViz.toggleSelected(i); },
  packClearSelections() { packViz.clearSelection(); },
  packNodeCount: () => packViz.nodeCount(),
  packNodeInfo: (i: number) => packViz.getNodeInfo(i),
  packDrillIn(i: number) { const r = packViz.drillIn(i); updateBreadcrumbAndHud(packEntry); return r; },
  packDrillOut() { const r = packViz.drillOut(); updateBreadcrumbAndHud(packEntry); return r; },
  lookAtPack(distance: number) {
    const v = new THREE.Vector3();
    packViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Sankey ---
  sankeyHover(i: number | null) {
    if (i === null) { nodeHoverFx.hide(); return; }
    sankeyViz.getNodeWorldPosition(i, tmpSankeyVec);
    nodeHoverFx.show(tmpSankeyVec, sankeyViz.getNodeLabel(i));
  },
  sankeyNodeCount: () => sankeyViz.nodeCount(),
  sankeyNodeInfo: (i: number) => sankeyViz.getNodeInfo(i),
  lookAtSankey(distance: number) {
    const v = new THREE.Vector3();
    sankeyViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Ridgeline ---
  ridgelineTick(t: number) { ridgelineViz.tick(t); },
};

window.addEventListener('keydown', e => {
  if (e.key === 'g' || e.key === 'G') showVizGallery(!vizAnchor.visible);
});

const toolbar = new Toolbar({
  buttons: [
    { id: 'gallery', label: 'Gallery', active: defaultToGallery,  onSelect: () => { showVizGallery(true);  placeAnchorInFrontOfUser(); } },
    { id: 'charts',  label: 'Charts',  active: !defaultToGallery, onSelect: () => { showVizGallery(false); placeAnchorInFrontOfUser(); } },
    { id: 'recenter', label: 'Recenter', onSelect: () => placeAnchorInFrontOfUser() },
    { id: 'floor', label: 'Set Floor', onSelect: () => placeFloorUnderHead() },
  ],
});
scene.add(toolbar.group);

for (const btn of toolbar.buttons) {
  const origBg = btn.block.backgroundColor?.clone?.() ?? new THREE.Color(0x0f1a2c);
  interact.add({
    id: `btn:${btn.id}`,
    object: btn.block,
    onHoverIn: () => {
      btn.block.set({ backgroundColor: new THREE.Color(0x2a4a6a), backgroundOpacity: 1.0 });
    },
    onHoverOut: (_ctx) => {
      btn.block.set({ backgroundColor: origBg, backgroundOpacity: 0.92 });
    },
    onSelect: () => btn.onSelect(),
  });
}

function placeToolbarNearUser() {
  const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
  const pos = new THREE.Vector3();
  xrCam.getWorldPosition(pos);
  const fwd = new THREE.Vector3(0, 0, -1).applyQuaternion(xrCam.quaternion);
  fwd.y = 0;
  if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
  fwd.normalize();
  const distance = 0.8;
  toolbar.group.position.set(
    pos.x + fwd.x * distance,
    pos.y - 0.35,
    pos.z + fwd.z * distance,
  );
  toolbar.group.lookAt(pos.x, toolbar.group.position.y, pos.z);
}
placeToolbarNearUser();

let lastT = 0;
renderer.setAnimationLoop((time, frame) => {
  const dt = lastT ? (time - lastT) / 1000 : 0;
  lastT = time;
  rig.update(frame);
  interact.update();
  if (vizAnchor.visible) {
    forceViz.tick();
    treeViz.tick();
    treemapViz.tick();
    sunburstViz.tick();
    packViz.tick();
    ridgelineViz.tick(time / 1000);
  }
  for (let hi = 0; hi < forceDragNodeIds.length; hi++) {
    const nodeId = forceDragNodeIds[hi];
    if (nodeId !== null && nodeId !== undefined) {
      forceViz.getNodeWorldPosition(nodeId, vecForHand(hi));
      fxForHand(hi).updatePosition(vecForHand(hi));
    }
  }
  for (const fx of nodeHoverFxs) fx.tick();
  ThreeMeshUI.update();
  updateDebugHud(dt);
  if (!anchorPlaced && !renderer.xr.isPresenting) {
    uiAnchor.position.set(0, 1.3, 0);
    uiAnchor.rotation.set(0, 0, 0);
  }
  renderer.render(scene, camera);
});
