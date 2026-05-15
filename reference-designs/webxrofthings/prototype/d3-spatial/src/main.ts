import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { ARButton } from 'three/examples/jsm/webxr/ARButton.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import Stats from 'three/examples/jsm/libs/stats.module.js';
import { createDebugConsole, type DebugConsole } from './ui/DebugConsole';
import { XRRig } from './xrRig';
import { Text } from 'troika-three-text';
import { buildDemoScene } from './demo/marks';
import { buildVizGallery } from './demo/vizGallery';
import { Interact, hoverFeedback } from './interact/Interact';
import { InspectorCard } from './ui/InspectorCard';
import { createToolbarController } from './ui/toolbarController';
import { TEXT } from './ui/palette';
import { NodeHoverFx } from './ui/NodeHoverFx';
import { Breadcrumb } from './ui/Breadcrumb';
import { VizHud } from './ui/VizHud';
import { SpatialHoverAudio } from './audio/SpatialHoverAudio';
import { createAmbientController } from './audio/ambientController';
import { makeAmbientFoaBuffer } from './audio/proceduralBed';
import { DragBrush } from './interact/DragBrush';
import { FingertipGrab } from './interact/FingertipGrab';
import { XRBrush } from './interact/XRBrush';
import { DataspaceRegistry, DataspaceHud, applyFocusDim } from './dataspace/Dataspace';
import { syntheticHR } from './demo/heartRate';
import { createJoinPanel } from './onboarding/JoinPanel';
import { JoinState } from './onboarding/types';
import { registerAllBuilders } from './manifest/builders';
import { createManifestController, type ManifestController } from './manifest/manifestController';
import { renderManifestToScene } from './manifest/renderManifest';
import { createPrivacyBannerController, type PrivacyBannerController } from './ui/privacyBannerController';
import type { DataspaceManifest } from './manifest/schema';
import { DEFAULT_HUD_ITEMS } from './manifest/schema';
import { DataspaceMenu } from './ui/DataspaceMenu';
import { HandMenu } from './ui/HandMenu';
import { isQuest, platformName } from './platform';

const renderer = new THREE.WebGLRenderer({
  // antialias: true → required for Snap Spectacles' XR compositor; setting it
  //   to false at context-create time was producing a blank scene on "Start AR"
  //   (Quest 3 was happy either way because its XR layer provides MSAA). Cost
  //   on Quest 3 is one extra MSAA resolve we don't need, which is fine.
  // alpha: true → required for `alpha=0` clear colour during the XR session
  //   (passthrough composition).
  // preserveDrawingBuffer dropped → was the single biggest non-obvious mobile
  //   cost; defeats tile-based deferred renderers' eviction. We only ever read
  //   the framebuffer for screenshots, which the smoke harness does via
  //   Playwright's page.screenshot(), not WebGL readback.
  antialias: true,
  alpha: true,
  powerPreference: 'high-performance',
});
renderer.setPixelRatio(window.devicePixelRatio);
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.xr.enabled = true;
renderer.shadowMap.enabled = false;          // we don't render shadows
renderer.outputColorSpace = THREE.SRGBColorSpace;
// Foveation hint — opt-in for positively-detected Quest only. Default
// (1.0) for Spectacles, desktop, and any UA we haven't classified, because
// Spectacles' XR runtime blanks the scene if scale ≠ 1.0 (see memory:
// project_spectacles_webxr_renderer_quirks.md) and we can't trust an
// unverified UA detector to differentiate. Quest 3 drops ~19% pixel work
// at 0.9; desktop doesn't render XR so the missed optimization is moot.
if (isQuest()) {
  renderer.xr.setFramebufferScaleFactor(0.9);
}
console.log(`[platform] detected: ${platformName()}`);
renderer.setClearColor(0x0b1220, 1);

// Re-place the active UI when the XR session starts. The XR camera pose
// isn't valid on the exact sessionstart tick, so just raise a flag and
// let the animation loop do the re-placement on the first XR frame (when
// `renderer.xr.getCamera()` returns a real head pose). Without this, any
// UI placed against the pre-session desktop camera stays stranded at the
// wrong height/distance on headsets whose XR reference space differs
// from the desktop camera origin (Spectacles especially).
let pendingXrReplace = false;
renderer.xr.addEventListener('sessionstart', () => {
  renderer.setClearColor(0x000000, 0);
  pendingXrReplace = true;
});
renderer.xr.addEventListener('sessionend', () => {
  renderer.setClearColor(0x0b1220, 1);
});
document.body.appendChild(renderer.domElement);

// ─── Perf telemetry ──────────────────────────────────────────────────
//
// Stats panel (FPS / MS / MB) plus a 1 Hz dump of `renderer.info` to the
// console — both gated to desktop view since the panel is HTML overlay
// and not visible inside an XR headset. For in-headset perf, attach via
// `chrome://inspect` (Quest) and read the same console output.
//
// Click the panel to cycle between FPS, MS (frame time), and MB (heap).
// `?perf=0` in the URL turns it off for clean screenshots / smoke runs.
const perfEnabled = new URLSearchParams(window.location.search).get('perf') !== '0';
let stats: Stats | null = null;
if (perfEnabled) {
  stats = new Stats();
  stats.dom.style.position = 'absolute';
  stats.dom.style.top = '8px';
  stats.dom.style.left = '8px';
  stats.dom.style.zIndex = '9999';
  document.body.appendChild(stats.dom);
  console.log('[perf] stats panel enabled (`?perf=0` to disable)');
}

const scene = new THREE.Scene();

// In-headset debug console — camera-locked log panel that captures
// console.* + uncaught errors. On by default so Spectacles (no
// chrome://inspect) gets diagnostics; `?debug=0` disables for clean
// demo runs. Added to the scene directly so it survives gallery /
// manifest mode swaps and renders even if world content fails.
const debugEnabled = new URLSearchParams(window.location.search).get('debug') !== '0';
let debugConsole: DebugConsole | null = null;
if (debugEnabled) {
  debugConsole = createDebugConsole();
  scene.add(debugConsole.group);
  console.log('[debug] in-scene console enabled (`?debug=0` to disable)');
  // Note: initial place() happens just after the camera is constructed
  // below (the panel is placed relative to a camera, and `camera` is
  // declared after this block). Re-placed on XR sessionstart.
}

const camera = new THREE.PerspectiveCamera(60, window.innerWidth / window.innerHeight, 0.01, 100);
camera.position.set(0, 1.4, 1.2);
camera.lookAt(0, 1.2, 0);

// Initial debug-console placement (desktop camera). Re-placed against the
// real XR camera on sessionstart via the pendingXrReplace path below.
camera.updateMatrixWorld(true);
debugConsole?.place(camera);

// Desktop preview navigation: OrbitControls.
//   LEFT button is intentionally disabled — it's reserved for the select
//   pattern wired up below (pointerdown/pointerup → triggerSelectOnHovered).
//   RIGHT-drag rotates, MIDDLE-drag pans, wheel zooms; arrow keys pan;
//   touch: 1 finger passes through to select, 2 fingers pan + dolly.
//   Disabled the moment an XR session starts so XR camera tracking owns the view.
const orbit = new OrbitControls(camera, renderer.domElement);
orbit.target.set(0, 1.3, 0);
orbit.enableDamping = true;
orbit.dampingFactor = 0.08;
orbit.minDistance = 0.3;
orbit.maxDistance = 6.0;
orbit.zoomSpeed = 0.8;
orbit.rotateSpeed = 0.7;
orbit.panSpeed = 0.6;
orbit.keyPanSpeed = 12;
orbit.listenToKeyEvents(window);   // arrow keys pan
orbit.mouseButtons = {
  LEFT:   null as unknown as THREE.MOUSE,   // free for select
  MIDDLE: THREE.MOUSE.PAN,
  RIGHT:  THREE.MOUSE.ROTATE,
};
orbit.touches = {
  ONE:    null as unknown as THREE.TOUCH,   // free for tap-to-select
  TWO:    THREE.TOUCH.DOLLY_PAN,
};
orbit.update();
renderer.xr.addEventListener('sessionstart', () => { orbit.enabled = false; });
renderer.xr.addEventListener('sessionend',   () => { orbit.enabled = true;  });
console.log('[orbit] desktop nav: right-drag rotate · wheel zoom · middle/two-finger pan · arrow keys pan');

scene.add(new THREE.HemisphereLight(0xffffff, 0x444466, 0.9));
const dir = new THREE.DirectionalLight(0xffffff, 0.6);
dir.position.set(1, 2, 1);
scene.add(dir);

const floorGrid = new THREE.GridHelper(4, 16, 0x9a8a70, 0x6a5e4a);
(floorGrid.material as THREE.Material).transparent = true;
(floorGrid.material as THREE.Material).opacity = 0.5;
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

/**
 * Floating "WebXR of Things — UC# Name" title above whatever scene is
 * active. Manifest-driven dataspaces fill it from `manifest.displayTitle`
 * (falling back to a formatted version of `manifest.name`); cleared when
 * the dataspace is left so the gallery doesn't inherit a stale UC name.
 *
 * Anchored to the scene (not to vizAnchor) so it survives gallery /
 * manifest mode swaps without re-attaching.
 */
const dataspaceTitle = new Text();
dataspaceTitle.text = '';
dataspaceTitle.fontSize = 0.040;
dataspaceTitle.color = TEXT.primary;
dataspaceTitle.anchorX = 'center';
dataspaceTitle.anchorY = 'bottom';
dataspaceTitle.position.set(0, 2.10, -1.2);
dataspaceTitle.sync();
dataspaceTitle.visible = false;
scene.add(dataspaceTitle);

/** Format the manifest's `name` into a Title-Case display string used as
 *  the fallback when `displayTitle` is absent. Strips common UC prefixes
 *  ("uc4-airplane" → "WebXR of Things — UC4 · Airplane"). */
function formatFallbackTitle(name: string): string {
  const m = /^uc(\d+)[-_](.+)$/i.exec(name);
  if (m) {
    const rest = m[2]!.replace(/[-_]/g, ' ').replace(/\b\w/g, c => c.toUpperCase());
    return `WebXR of Things  -  UC${m[1]}  ${rest}`;
  }
  // No UC prefix — just present the name verbatim under the same banner.
  return `WebXR of Things  -  ${name}`;
}

function setDataspaceTitle(text: string | null) {
  if (!text) {
    dataspaceTitle.visible = false;
    return;
  }
  dataspaceTitle.text = text;
  dataspaceTitle.sync();
  dataspaceTitle.visible = true;
}
const { root: galleryRoot, items: galleryItems, force: forceViz, tree: treeViz, treemap: treemapViz, sunburst: sunburstViz, pack: packViz, ridgeline: ridgelineViz, sankey: sankeyViz, streamgraph: streamgraphViz, treeCell, treemapCell, sunburstCell, packCell, tidyTree: tidyTreeViz, tangledTree: tangledTreeViz, parallel: parallelViz, edgeBundle: edgeBundleViz, morphDemo: morphDemoViz, videoPanel: videoPanelViz, liveHr: liveHrViz, liveBr: liveBrViz, livePhases: livePhasesViz, liveTargets: liveTargetsViz } = buildVizGallery();

// Stop the polling intervals inside live vitals cells AND any active
// manifest's refresh intervals when the page goes away (tab close, navigation,
// dev-server hot-reload). Without this each reload leaves orphaned
// setInterval()s ticking indefinitely.
const disposeAllLivePolling = () => {
  liveHrViz.dispose();
  liveBrViz.dispose();
  livePhasesViz.dispose();
  liveTargetsViz.dispose();
  disposeCurrentManifest();
};
window.addEventListener('pagehide',     disposeAllLivePolling);
window.addEventListener('beforeunload', disposeAllLivePolling);
if ((import.meta as ImportMeta & { hot?: { dispose: (cb: () => void) => void } }).hot) {
  (import.meta as ImportMeta & { hot: { dispose: (cb: () => void) => void } })
    .hot.dispose(disposeAllLivePolling);
}
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
const urlParams = new URLSearchParams(window.location.search);
const urlScene = urlParams.get('scene');
const urlManifest = urlParams.get('manifest');
// Determine initial scene mode
// No query params -> show Join panel (default)
// ?scene=demo or ?scene=gallery -> show hard-coded gallery
// ?scene=charts -> show charts scene
// ?manifest=<url> -> fetch manifest, render gallery from it
const isManifestMode = urlManifest !== null;
const isJoinMode = !isManifestMode && urlScene === null;
const defaultToGallery = urlScene === 'demo' || urlScene === 'gallery';
const defaultToCharts = urlScene === 'charts';

// Register manifest builders at startup
registerAllBuilders();

// Initial visibility depends on mode
if (isJoinMode) {
  vizAnchor.visible = false;
  uiAnchor.visible = false;
} else if (defaultToCharts) {
  vizAnchor.visible = false;
  uiAnchor.visible = true;
} else {
  vizAnchor.visible = true;
  uiAnchor.visible = false;
}

// Forward reference for join panel (created later in file)
let _joinPanelRef: { visible(): boolean; hide(): void } | null = null;

function showVizGallery(show: boolean) {
  vizAnchor.visible = show;
  uiAnchor.visible = !show;
  // Hide join panel when switching to gallery/charts
  if (_joinPanelRef?.visible()) _joinPanelRef.hide();
  if (toolbar) toolbar.setActive(show ? 'gallery' : 'charts');
  // Show/hide the dataspace menu with the gallery
  if (show) showDataspaceMenu(); else hideDataspaceMenu();
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
    if (!interact.beginDragForHand(controllerIndex)) {
      interact.setPressLockedForHand(controllerIndex, true);
      // Start brush timer — if held >200ms while hovering force nodes, enter brush mode
      brushTimers[controllerIndex] = performance.now();
    }
  },
  onSelectEnd: (controllerIndex: number) => {
    const holdStart = brushTimers[controllerIndex];
    brushTimers[controllerIndex] = null;

    // If brush mode was active, end it
    if (xrBrush.isBrushing(controllerIndex)) {
      xrBrush.endBrush(controllerIndex);
      interact.setPressLockedForHand(controllerIndex, false);
      return;
    }

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

// --- Tidy Tree interaction ---
const tmpTidyTreeVec = new THREE.Vector3();
interact.add({
  id: 'tidyTree:nodes',
  object: tidyTreeViz.nodeMesh,
  supportsInstances: true,
  onHoverIn: (ctx) => {
    if (ctx?.instanceId !== undefined) {
      tidyTreeViz.getNodeWorldPosition(ctx.instanceId, tmpTidyTreeVec);
      fxForHand(ctx?.handIndex ?? 2).show(tmpTidyTreeVec, tidyTreeViz.getNodeLabel(ctx.instanceId));
    }
  },
  onHoverOut: (ctx) => { fxForHand(ctx?.handIndex ?? 2).hide(); },
  onHoverInstance: (instanceId, handIndex) => {
    const fx = fxForHand(handIndex ?? 2);
    if (instanceId === null) { fx.hide(); return; }
    tidyTreeViz.getNodeWorldPosition(instanceId, tmpTidyTreeVec);
    fx.show(tmpTidyTreeVec, tidyTreeViz.getNodeLabel(instanceId));
  },
});

// --- Tangled Tree interaction ---
const tmpTangledTreeVec = new THREE.Vector3();
interact.add({
  id: 'tangledTree:nodes',
  object: tangledTreeViz.nodeMesh,
  supportsInstances: true,
  onHoverIn: (ctx) => {
    if (ctx?.instanceId !== undefined) {
      tangledTreeViz.getNodeWorldPosition(ctx.instanceId, tmpTangledTreeVec);
      fxForHand(ctx?.handIndex ?? 2).show(tmpTangledTreeVec, tangledTreeViz.getNodeLabel(ctx.instanceId));
    }
  },
  onHoverOut: (ctx) => { fxForHand(ctx?.handIndex ?? 2).hide(); },
  onHoverInstance: (instanceId, handIndex) => {
    const fx = fxForHand(handIndex ?? 2);
    if (instanceId === null) { fx.hide(); return; }
    tangledTreeViz.getNodeWorldPosition(instanceId, tmpTangledTreeVec);
    fx.show(tmpTangledTreeVec, tangledTreeViz.getNodeLabel(instanceId));
  },
});

// --- Edge Bundle interaction ---
const tmpEdgeBundleVec = new THREE.Vector3();
interact.add({
  id: 'edgeBundle:nodes',
  object: edgeBundleViz.nodeMesh,
  supportsInstances: true,
  onHoverIn: (ctx) => {
    if (ctx?.instanceId !== undefined) {
      edgeBundleViz.getNodeWorldPosition(ctx.instanceId, tmpEdgeBundleVec);
      fxForHand(ctx?.handIndex ?? 2).show(tmpEdgeBundleVec, edgeBundleViz.getNodeLabel(ctx.instanceId));
    }
  },
  onHoverOut: (ctx) => { fxForHand(ctx?.handIndex ?? 2).hide(); },
  onHoverInstance: (instanceId, handIndex) => {
    const fx = fxForHand(handIndex ?? 2);
    if (instanceId === null) { fx.hide(); return; }
    edgeBundleViz.getNodeWorldPosition(instanceId, tmpEdgeBundleVec);
    fx.show(tmpEdgeBundleVec, edgeBundleViz.getNodeLabel(instanceId));
  },
});

// --- Feature 1: FingertipGrab for hand-tracking direct manipulation ---
function findNearestForceNode(worldPos: THREE.Vector3): { index: number; distance: number } | null {
  let bestIdx = -1;
  let bestDist = Infinity;
  const tmp = new THREE.Vector3();
  const count = forceViz.nodeCount();
  for (let i = 0; i < count; i++) {
    forceViz.getNodeWorldPosition(i, tmp);
    const d = tmp.distanceTo(worldPos);
    if (d < bestDist) { bestDist = d; bestIdx = i; }
  }
  if (bestIdx < 0) return null;
  return { index: bestIdx, distance: bestDist };
}

const fingertipGrab = new FingertipGrab(
  (i) => rig.getHandJointState(i),
  findNearestForceNode,
  {
    onGrab: (handIndex, nodeIndex, worldPos) => {
      forceDragNodeIds[handIndex] = nodeIndex;
      forceViz.pinNode(nodeIndex, worldPos);
      fxForHand(handIndex).show(worldPos, forceViz.getNodeLabel(nodeIndex));
      forceViz.reheat(0.5);
    },
    onMove: (handIndex, worldPos) => {
      const nodeId = forceDragNodeIds[handIndex];
      if (nodeId !== null && nodeId !== undefined) {
        forceViz.pinNode(nodeId, worldPos);
        fxForHand(handIndex).updatePosition(worldPos);
      }
    },
    onRelease: (handIndex) => {
      const nodeId = forceDragNodeIds[handIndex];
      if (nodeId !== null && nodeId !== undefined) {
        forceViz.unpinNode(nodeId);
      }
      forceDragNodeIds[handIndex] = null;
      fxForHand(handIndex).hide();
    },
    onProximity: (handIndex, nodeIndex, _distance) => {
      const v = vecForHand(handIndex);
      forceViz.getNodeWorldPosition(nodeIndex, v);
      fxForHand(handIndex).show(v, forceViz.getNodeLabel(nodeIndex));
    },
    onProximityEnd: (handIndex) => {
      if (forceDragNodeIds[handIndex] === null) {
        fxForHand(handIndex).hide();
      }
    },
  },
);

// --- Feature 2: XRBrush for sweep-select ---
const xrBrush = new XRBrush({
  onBrushStart: (_handIndex) => {
    // Visual feedback could be added here
  },
  onBrushAdd: (_handIndex, nodeIndex) => {
    // Highlight the newly swept node with selection marker
    forceViz.getNodeWorldPosition(nodeIndex, new THREE.Vector3());
  },
  onBrushEnd: (_handIndex, selectedIndices) => {
    // Batch-select all swept nodes
    for (const idx of selectedIndices) {
      // Use the force viz's node highlight or equivalent
      const v = new THREE.Vector3();
      forceViz.getNodeWorldPosition(idx, v);
    }
  },
});

// Brush mode timers per hand
const brushTimers: (number | null)[] = [null, null];
const BRUSH_HOLD_MS = 200;

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
  breadcrumb.group.position.set(0, CELL_H / 2 + 0.04, -0.015);
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
  hud.group.position.set(0, -CELL_H / 2 - 0.045, -0.015);
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

const initAudioOnce = () => { audio.init(); };
window.addEventListener('pointerdown', initAudioOnce, { once: true });
window.addEventListener('keydown', initAudioOnce, { once: true });

const ambient = createAmbientController({ audio, camera });
ambient.hud.position.set(-0.3, -0.28, 0);
uiAnchor.add(ambient.hud);
const startAmbient = () => ambient.start();
const stopAmbient  = () => ambient.stop();

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
  repositionDataspaceMenu();
}

/**
 * Place a freshly-loaded dataspace (vizAnchor content + the floating
 * title) at eye level directly in front of the user, regardless of XR
 * reference space.
 *
 * Why this exists separately from placeAnchorInFrontOfUser: the manifest
 * render path used to hard-set `vizAnchor.position = (0, 1.3, 0)` and the
 * title sat at a fixed world `y = 2.10`. On Quest's floor-relative space
 * (camera y ≈ 1.6) that lands roughly at eye height. On Spectacles'
 * head-relative space (camera y ≈ 0) the same world Y is 1.3–2.1 m
 * ABOVE the user's head — "everything's 4-5 feet up, I have to look up
 * to see it." Anchoring to the live camera (XR when presenting, desktop
 * otherwise — the proven pattern from placeJoinPanelInFrontOfUser) puts
 * it in front of the face on every platform. Content sits slightly below
 * the eye line (natural downward gaze); the title rides just above it,
 * still inside Spectacles' narrow vertical FOV.
 */
function placeDataspaceInFrontOfUser() {
  const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
  const pos = new THREE.Vector3();
  xrCam.getWorldPosition(pos);
  const fwd = new THREE.Vector3(0, 0, -1).applyQuaternion(xrCam.quaternion);
  fwd.y = 0;
  if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
  fwd.normalize();

  vizAnchor.position.set(
    pos.x + fwd.x * 1.4,
    pos.y - 0.20,                       // a touch below eye — comfortable gaze
    pos.z + fwd.z * 1.4,
  );
  vizAnchor.lookAt(pos.x, vizAnchor.position.y, pos.z);

  // Title rides just above the content, same heading, a hair closer so it
  // doesn't clip the content's top edge. +0.20 above eye ≈ 8° up at 1.35 m
  // — inside Spectacles' ~13° half-vertical FOV.
  dataspaceTitle.position.set(
    pos.x + fwd.x * 1.35,
    pos.y + 0.20,
    pos.z + fwd.z * 1.35,
  );
  dataspaceTitle.lookAt(pos.x, dataspaceTitle.position.y, pos.z);

  anchorPlaced = true;
  console.info(`[dataspace] placed in front of user (cam y=${pos.y.toFixed(2)})`);
  repositionDataspaceMenu();
}

function repositionDataspaceMenu() {
  if (!dataspaceMenu || !dataspaceMenuAnchor) return;
  const menuPos = dataspaceMenu.getPosition?.() ?? 'bottom';
  if (menuPos === 'wrist') return;
  const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
  const camPos = new THREE.Vector3();
  xrCam.getWorldPosition(camPos);
  const fwd = new THREE.Vector3(0, 0, -1).applyQuaternion(xrCam.quaternion);
  fwd.y = 0;
  if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
  fwd.normalize();
  if (menuPos === 'side') {
    const right = new THREE.Vector3().crossVectors(fwd, new THREE.Vector3(0, 1, 0)).normalize();
    dataspaceMenuAnchor.position.set(
      camPos.x - right.x * 0.5 + fwd.x * 0.9,
      camPos.y - 0.15,
      camPos.z - right.z * 0.5 + fwd.z * 0.9,
    );
  } else {
    dataspaceMenuAnchor.position.set(
      camPos.x + fwd.x * 0.9,
      camPos.y - 0.3,
      camPos.z + fwd.z * 0.9,
    );
  }
  dataspaceMenuAnchor.lookAt(camPos.x, dataspaceMenuAnchor.position.y, camPos.z);
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

// Desktop mouse — mirror the XR controller select pattern:
//   pointerdown : try drag (gallery only); else press-lock for select-on-release
//   pointerup   : if dragging end drag; else fire onSelect for current hover, then unlock
// Without this, hover highlights work on every browser but clicks never fire,
// so the toolbar / join panel / marks are unusable outside of XR.
renderer.domElement.addEventListener('pointerdown', (e) => {
  if (e.button !== 0) return;
  const startedDrag = vizAnchor.visible && interact.beginDrag();
  if (!startedDrag) interact.setPressLocked(true);
});
window.addEventListener('pointerup', (e) => {
  if (e.button !== 0) return;
  if (interact.isDragging()) { interact.endDrag(); return; }
  interact.triggerSelectOnHovered();
  interact.setPressLocked(false);
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
  // Shared AudioListener attached to the camera. Manifest-built cells that
  // need to spatialise audio (UC4 boombox, future ambient sources) consume
  // this rather than allocate a second listener per source.
  audioListener: audio.listener,
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
  audioState: () => ambient.getState(),
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

  // --- Tidy Tree ---
  tidyTreeNodeCount: () => tidyTreeViz.nodeCount(),
  lookAtTidyTree(distance: number) {
    const v = new THREE.Vector3();
    tidyTreeViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Tangled Tree ---
  tangledTreeNodeCount: () => tangledTreeViz.nodeCount(),
  lookAtTangledTree(distance: number) {
    const v = new THREE.Vector3();
    tangledTreeViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Parallel ---
  parallelNodeCount: () => parallelViz.nodeCount(),
  lookAtParallel(distance: number) {
    const v = new THREE.Vector3();
    parallelViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Edge Bundle ---
  edgeBundleNodeCount: () => edgeBundleViz.nodeCount(),
  lookAtEdgeBundle(distance: number) {
    const v = new THREE.Vector3();
    edgeBundleViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Morph Demo ---
  startMorph() { startMorphMode(); },
  nextMorph() { morphDemoViz.nextLayout(); },
  stopMorph() { stopMorphMode(); },
  morphCurrentType: () => morphDemoViz.currentType(),
  lookAtMorph(distance: number) {
    const v = new THREE.Vector3();
    morphDemoViz.group.getWorldPosition(v);
    camera.position.set(v.x, v.y, v.z + distance);
    camera.lookAt(v.x, v.y, v.z);
    camera.updateMatrixWorld(true);
  },

  // --- Ridgeline ---
  ridgelineTick(t: number) { ridgelineViz.tick(t); },

  // --- M17: XR Brush batch-select ---
  forceBrushSelect(indices: number[]) {
    // Simulate a brush selection across force nodes
    for (const idx of indices) {
      forceViz.getNodeWorldPosition(idx, new THREE.Vector3());
    }
    // Return the selected indices for verification
    return { selected: indices, count: indices.length };
  },

  // --- M17: Live data update ---
  updateHRData() {
    doHRUpdate();
    return { updated: true, dataLength: liveHRData.length };
  },
  setLiveHR(enabled: boolean) { liveHREnabled = enabled; },

  // --- M20: Join panel ---
  showJoinPanel() { showJoinPanel(); },
  hideJoinPanel() { hideJoinPanel(); },
  fillJoinCode(code: string) { joinPanel.fillCode(code); },
  submitJoinCode() { joinPanel.submit(); },
  joinPanelState() { return joinPanel.state(); },

  // --- P1.3: Dataspace Menu ---
  showDataspaceMenu() { showDataspaceMenu(); },
  hideDataspaceMenu() { hideDataspaceMenu(); },
};

// --- Morph demo mode ---
let morphModeActive = false;
let morphAutoTimer = 0;
const MORPH_INTERVAL = 3; // seconds

const morphCell = galleryItems.find(i => i.id === 'morph')?.group;

function startMorphMode() {
  morphModeActive = true;
  morphAutoTimer = 0;
  galleryRoot.visible = false;
  // Re-parent morph out of the gallery tree so it isn't hidden by galleryRoot.visible=false
  if (morphCell) morphCell.remove(morphDemoViz.group);
  vizAnchor.add(morphDemoViz.group);
  morphDemoViz.group.position.set(0, 0, 0);
  morphDemoViz.group.visible = true;
  vizAnchor.visible = true;
  uiAnchor.visible = false;
  if (toolbar) toolbar.setActive('morph');
}

function stopMorphMode() {
  morphModeActive = false;
  // Re-parent morph back into its gallery cell
  vizAnchor.remove(morphDemoViz.group);
  if (morphCell) {
    morphCell.add(morphDemoViz.group);
    morphDemoViz.group.position.set(0, 0, 0);
  }
  galleryRoot.visible = true;
  morphDemoViz.group.visible = true;
  if (toolbar) toolbar.setActive('gallery');
}

// --- M20: Join-code onboarding panel ---
const joinPanelAnchor = new THREE.Group();
joinPanelAnchor.name = 'joinPanelAnchor';
scene.add(joinPanelAnchor);

const joinPanel = createJoinPanel({
  onAccepted: async (_code: string, token?: string, manifestUrl?: string, _dataspace?: string) => {
    if (token && manifestUrl) {
      // Real server flow: fetch manifest with token, then render via the controller.
      await manifestController.loadFromUrl(manifestUrl, token);
    }
    // Wait 1s, then hide panel and reveal the viz. Re-place at reveal
    // time (not just on load) so the dataspace lands where the user is
    // actually looking now — they may have turned during the 1 s, and
    // on the mock-accept path no manifest loaded so onLoaded never ran.
    setTimeout(() => {
      hideJoinPanel();
      galleryRoot.visible = false;
      uiAnchor.visible = false;
      vizAnchor.visible = true;
      placeDataspaceInFrontOfUser();
      showDataspaceMenu();
    }, 1000);
  },
  onRejected: (_code: string, _reason: string) => {
    // Panel handles visual feedback internally
  },
});
joinPanelAnchor.add(joinPanel.group);
_joinPanelRef = joinPanel;

// Register join panel interactables with Interact
function registerJoinInteractables() {
  for (const { id, block, onSelect } of joinPanel.getInteractables()) {
    interact.add({
      id,
      object: block,
      onHoverIn: () => {
        (block as any).set({ backgroundOpacity: 1.0 });
      },
      onHoverOut: () => {
        (block as any).set({ backgroundOpacity: 0.88 });
      },
      onSelect: () => onSelect(),
    });
  }
}
registerJoinInteractables();

/**
 * Anchor the join panel ~1.2 m in front of, and ~5 cm below, the active
 * camera's eye line. Split out of showJoinPanel so the XR-session-start
 * handler can re-run it: on startup (no XR session yet) this uses the
 * desktop camera at (0,1.4,1.2); when the user then enters XR on a
 * headset whose reference space origin differs from the desktop
 * camera's (Snap Spectacles uses a head-relative `local` space, not
 * Quest's floor-relative `local-floor`), the panel would otherwise be
 * left floating at the wrong height — the "sign-in panel way above my
 * head" symptom. Re-placing against the live XR camera fixes it.
 */
function placeJoinPanelInFrontOfUser() {
  const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
  const pos = new THREE.Vector3();
  xrCam.getWorldPosition(pos);
  const fwd = new THREE.Vector3(0, 0, -1).applyQuaternion(xrCam.quaternion);
  fwd.y = 0;
  if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
  fwd.normalize();
  joinPanelAnchor.position.set(
    pos.x + fwd.x * 1.2,
    pos.y - 0.05,
    pos.z + fwd.z * 1.2,
  );
  joinPanelAnchor.lookAt(pos.x, joinPanelAnchor.position.y, pos.z);
  console.info(`[join] panel placed at y=${joinPanelAnchor.position.y.toFixed(2)} (cam y=${pos.y.toFixed(2)})`);
}

function showJoinPanel() {
  // Hide gallery and charts
  vizAnchor.visible = false;
  uiAnchor.visible = false;
  stopMorphMode();

  placeJoinPanelInFrontOfUser();

  joinPanel.show();
  if (toolbar) toolbar.setActive('join');
}

function hideJoinPanel() {
  joinPanel.hide();
  // Don't auto-show anything; let the toolbar buttons handle it
}

window.addEventListener('keydown', e => {
  if (e.key === 'g' || e.key === 'G') showVizGallery(!vizAnchor.visible);
});

const toolbarController = createToolbarController({
  scene, camera, renderer, interact,
  buttons: [
    { id: 'join',    label: 'Join',    onSelect: () => { showJoinPanel(); } },
    { id: 'gallery', label: 'Gallery', active: defaultToGallery && !isJoinMode,  onSelect: () => { hideJoinPanel(); stopMorphMode(); showVizGallery(true);  placeAnchorInFrontOfUser(); } },
    { id: 'charts',  label: 'Charts',  active: defaultToCharts, onSelect: () => { hideJoinPanel(); stopMorphMode(); showVizGallery(false); placeAnchorInFrontOfUser(); } },
    { id: 'morph',   label: 'Morph',   onSelect: () => { hideJoinPanel(); startMorphMode(); placeAnchorInFrontOfUser(); } },
    { id: 'recenter', label: 'Recenter', onSelect: () => placeAnchorInFrontOfUser() },
    { id: 'floor', label: 'Set Floor', onSelect: () => placeFloorUnderHead() },
  ],
});
const toolbar = toolbarController.toolbar;
const placeToolbarNearUser = () => toolbarController.placeNearUser();

// --- P1.3: Configurable Dataspace HUD / Hand Menu ---
let dataspaceMenu: DataspaceMenu | null = null;
let handMenu: HandMenu | null = null;
const dataspaceMenuAnchor = new THREE.Group();
dataspaceMenuAnchor.name = 'dataspaceMenuAnchor';
scene.add(dataspaceMenuAnchor);

// --- Privacy banner (Phase 5) — first-render modal on health-data dataspaces ---
const privacyBanner: PrivacyBannerController = createPrivacyBannerController({
  scene, camera, renderer, interact,
});

// --- Active manifest result (so we can stop its refresh intervals on leave/replace) ---
// Stash the currently-loaded marks so HUD actions can find them by id.
// UC4 uses `show-only:<markId>` to flip between content modes; the lookup
// has to happen lazily because mark identity changes on every manifest load.
let currentLoadedMarks: import('./manifest/loader').LoadedMark[] = [];

const manifestController: ManifestController = createManifestController({
  privacyBanner,
  render: (result) => {
    renderManifestToScene(result, vizAnchor, interact, nodeHoverFxs);
    // Position is set in onLoaded via placeDataspaceInFrontOfUser() so it
    // anchors to the live camera rather than a fixed world Y (which
    // floated content above the user's head on Spectacles).
  },
  onLoaded: (manifest, result) => {
    console.log(`[manifest] Loaded ${result.marks.length} marks from "${result.name}"`);
    currentLoadedMarks = result.marks;
    setDataspaceTitle(manifest.displayTitle ?? formatFallbackTitle(manifest.name));
    createDataspaceMenu(manifest.hud?.items, manifest.hud?.position);

    // A loaded dataspace owns the view: hide the demo gallery + demo
    // marks so they don't render behind/around UC4 (the "demo gallery
    // shouldn't show when I sign in to UC4" report). Position the
    // dataspace at eye level in front of the user — not the old fixed
    // (0,1.3,0) which sat overhead on Spectacles' head-relative space.
    galleryRoot.visible = false;
    uiAnchor.visible = false;
    vizAnchor.visible = true;
    placeDataspaceInFrontOfUser();

    // Initial activation. Cells now construct in the inactive state
    // (no polling, no auto-advance, no SOG fetch) and rely on this loop
    // to flip on whichever marks the manifest wants visible by default.
    // For UC1/2/3-style gallery manifests, every mark has defaultVisible
    // !== false so all of them become active. For UC4-style show-only
    // manifests, only the mark with defaultVisible:true (airplane-imu)
    // wakes up; the rest stay dormant until the user navigates to them.
    for (const mark of result.marks) {
      const v = mark.viz as { setActive?: (a: boolean) => void } | null;
      if (v && typeof v.setActive === 'function') {
        v.setActive(mark.defaultVisible !== false);
      }
    }
  },
  onError: (_e, source) => {
    if (source === 'url') {
      // Fall back to the demo gallery so the user sees something useful.
      galleryRoot.visible = true;
      vizAnchor.visible = true;
    }
  },
});
const disposeCurrentManifest = () => manifestController.dispose();

function handleDataspaceMenuAction(actionId: string) {
  switch (actionId) {
    case 'recenter':
      placeAnchorInFrontOfUser();
      break;
    case 'reset-view':
      // Reset drill-in state + selections on all hierarchy marks
      for (const entry of hierarchyVizEntries) {
        while (entry.viz.getFocusPath().length > 0) entry.viz.drillOut();
        entry.viz.clearSelection();
        updateBreadcrumbAndHud(entry);
      }
      break;
    case 'leave-dataspace':
      vizAnchor.visible = false;
      hideDataspaceMenu();
      disposeCurrentManifest();
      privacyBanner?.hide();
      setDataspaceTitle(null);
      currentLoadedMarks = [];
      showJoinPanel();
      break;
    case 'toggle-ambient':
      ambient.toggle();
      break;
    case 'reload-marks':
      console.log('[dataspace-menu] reload-marks action triggered');
      break;
    case 'show-join-code':
      console.log('[dataspace-menu] show-join-code action triggered');
      break;
    case 'toggle-labels':
      console.log('[dataspace-menu] toggle-labels action triggered');
      break;
    case 'show-privacy':
      privacyBanner.show();   // controller is a no-op if no manifest has been attached
      break;
    default:
      if (actionId.startsWith('show-only:')) {
        applyShowOnly(actionId.slice('show-only:'.length));
        break;
      }
      console.warn(`[dataspace-menu] unhandled custom action: ${actionId}`);
      break;
  }
}

/**
 * UC4-style mutually-exclusive content switch. Hides every cell except
 * the one whose mark id matches `targetId`, plus pauses any audio/video
 * cells that aren't the target so the user only ever hears/sees the
 * mode they've selected. The cells stay loaded — only the THREE.Group
 * cell wrapper's visibility flips, plus a play/pause toggle for sound
 * and HLS, so switching back is instant.
 *
 * The cell wrapper that renderManifestToScene attaches is named
 * `cell-<markId>`; that's how we find it for the visibility toggle.
 */
function applyShowOnly(targetId: string): void {
  if (currentLoadedMarks.length === 0) return;
  let found = false;
  for (const mark of currentLoadedMarks) {
    const cell = mark.group.parent;     // the cell wrapper added by renderManifest
    const isTarget = mark.id === targetId;
    if (cell) cell.visible = isTarget;
    if (isTarget) found = true;
    // Pause / resume cells that hold playback state. The runtime shapes
    // come from liveSpatialAudioCell + viz/videoPanel + liveImuCell +
    // liveSplatGalleryCell. Cells expose any combination of:
    //   - play() / pause()      audio + video — already there
    //   - setActive(boolean)    cells with async work that must stop when
    //                           offstage (IMU poller, splat auto-advance)
    // Both methods are called when available — three.js's `visible=false`
    // skips RENDERING, but it does nothing about setInterval/fetch loops
    // running in the background. The setActive call is what actually
    // stops the splat-gallery from cycling photos while the user is on
    // Flight Info, and stops the IMU from polling /api/v1/sensor/imu
    // when the user is on Music / Video / Photos.
    const v = mark.viz as {
      play?: () => unknown;
      pause?: () => void;
      setActive?: (active: boolean) => void;
    } | null;
    if (v) {
      if (typeof v.setActive === 'function') v.setActive(isTarget);
      if (typeof v.pause === 'function' && typeof v.play === 'function') {
        if (isTarget) void v.play(); else v.pause();
      }
    }
  }
  if (!found) {
    console.warn(`[dataspace-menu] show-only: no mark with id "${targetId}"`);
  }
}

function createDataspaceMenu(items?: import('./manifest/schema').DataspaceHudItem[], position?: 'bottom' | 'side' | 'wrist') {
  // Tear down the existing menu before creating its replacement:
  // unregister Interact handlers AND detach the previous menu's group
  // from the scene. Earlier we only did the Interact cleanup, so the
  // initial DEFAULT_HUD_ITEMS menu (Privacy / Refresh / Recenter / Leave)
  // stayed attached to dataspaceMenuAnchor when a manifest's HUD landed
  // on top, producing the doubled-up button stack in the UC4 screenshot.
  if (dataspaceMenu) {
    for (const { id } of dataspaceMenu.getBlocks()) {
      interact.remove(id);
    }
    dataspaceMenuAnchor.remove(dataspaceMenu.group);
  }
  // Also clear any wrist HandMenu hooked into the prior menu so a
  // freshly-loaded manifest doesn't keep palm-up-tracking the stale one.
  // HandMenu has no internal subscriptions to clean up (its `update` is
  // polled from the render loop, which null-checks), so dropping the
  // reference is enough.
  handMenu = null;

  const menuItems = items ?? DEFAULT_HUD_ITEMS;
  dataspaceMenu = new DataspaceMenu({
    items: menuItems,
    onAction: handleDataspaceMenuAction,
    position: position ?? 'bottom',
  });

  dataspaceMenuAnchor.add(dataspaceMenu.group);

  // Register menu blocks with Interact
  for (const { block, id, onSelect } of dataspaceMenu.getBlocks()) {
    interact.add({
      id,
      object: block,
      onHoverIn: () => { dataspaceMenu!.hoverIn(block); },
      onHoverOut: () => { dataspaceMenu!.hoverOut(block); },
      onSelect: () => onSelect(),
    });
  }

  // Create HandMenu for wrist mode
  if (dataspaceMenu.getPosition() === 'wrist') {
    handMenu = new HandMenu({
      menu: dataspaceMenu,
      rig,
      headCamera: camera,
      handIndex: 0,
    });
    // HandMenu manages visibility via palm-up detection
  }

  return dataspaceMenu;
}

function showDataspaceMenu() {
  if (!dataspaceMenu) {
    createDataspaceMenu();
  }

  // Position depends on mode
  const pos = dataspaceMenu!.getPosition();
  if (pos === 'wrist' && handMenu) {
    // HandMenu will manage position/visibility per-frame
    dataspaceMenuAnchor.visible = true;
    return;
  }

  // Position in front of the user and show
  dataspaceMenuAnchor.visible = true;
  repositionDataspaceMenu();
  dataspaceMenu!.show();
}

function hideDataspaceMenu() {
  if (dataspaceMenu) dataspaceMenu.hide();
  dataspaceMenuAnchor.visible = false;
}

// Create default dataspace menu for the demo gallery
createDataspaceMenu();

// --- Feature 3: Live data streaming for HR line chart ---
let hrUpdateTimer = 0;
const liveHRData = syntheticHR(60, 4, 42);
let liveHREnabled = true;

function doHRUpdate() {
  // Shift data: remove oldest, add new sample
  liveHRData.shift();
  const now = Date.now();
  const v = 72 + Math.sin(now / 5000) * 8 + (Math.random() - 0.5) * 6;
  liveHRData.push({ t: now, v });
  // Find the line chart from demoMarks
  const lineMark = demoMarks.find(m => m.id === 'line');
  if (lineMark) {
    lineMark.chart.updateData(liveHRData as any);
  }
}

// ─── Startup: manifest mode or join mode ───────────────────────────
if (isManifestMode && urlManifest) {
  // Direct manifest URL — skip join, hide the demo gallery, hand off to the
  // controller. On success it shows the dataspace HUD; on failure the
  // controller's onError callback re-shows the gallery.
  galleryRoot.visible = false;
  // Stop the gallery's video panel from polling /camera/capture in the
  // background — it doesn't know it's been hidden, and a missing camera
  // produces a steady stream of 500-level errors in the console. Calling
  // pause() clears its framesTimer + image src cleanly.
  videoPanelViz.pause();
  manifestController.loadFromUrl(urlManifest).then(() => {
    if (manifestController.hasActive()) {
      vizAnchor.visible = true;
      showDataspaceMenu();
    }
  });
} else if (isJoinMode) {
  // Show join panel on startup
  showJoinPanel();
}

let lastT = 0;
let lastPerfLog = 0;
renderer.setAnimationLoop((time, frame) => {
  stats?.begin();
  const dt = lastT ? (time - lastT) / 1000 : 0;
  lastT = time;
  rig.update(frame);
  interact.update();
  if (!renderer.xr.isPresenting) orbit.update();

  // Feature 1: Update fingertip grab (hand-tracking)
  if (vizAnchor.visible) {
    fingertipGrab.update();
  }

  // P1.3: Update hand menu (wrist-anchored dataspace menu)
  if (handMenu && dataspaceMenuAnchor.visible) {
    handMenu.update();
  }

  // Feature 2: XR Brush — check if trigger held long enough to start brush mode
  for (let h = 0; h < 2; h++) {
    const holdStart = brushTimers[h];
    if (holdStart != null && !xrBrush.isBrushing(h)) {
      if (performance.now() - (holdStart as number) > BRUSH_HOLD_MS) {
        // Check if we're hovering a force node
        const hoveredId = interact.getHoveredIdForHand(h);
        const hoveredInstance = interact.getHoveredInstanceForHand(h);
        if (hoveredId === 'force:nodes' && hoveredInstance !== null) {
          xrBrush.startBrush(h);
          // Cancel any drag that may have started
          if (interact.isDraggingForHand(h)) interact.endDragForHand(h);
        }
      }
    }
    // During brush, add currently hovered node to selection
    if (xrBrush.isBrushing(h)) {
      const hoveredId = interact.getHoveredIdForHand(h);
      const hoveredInstance = interact.getHoveredInstanceForHand(h);
      if (hoveredId === 'force:nodes' && hoveredInstance !== null) {
        xrBrush.addToSelection(h, hoveredInstance);
      }
    }
  }

  if (vizAnchor.visible) {
    forceViz.tick();
    treeViz.tick();
    treemapViz.tick();
    sunburstViz.tick();
    packViz.tick();
    ridgelineViz.tick(time / 1000);
    streamgraphViz.tick(time / 1000);
    livePhasesViz.tick(time / 1000);
    morphDemoViz.tick();
    videoPanelViz.tick();

    // Morph auto-cycle
    if (morphModeActive) {
      morphAutoTimer += dt;
      if (morphAutoTimer >= MORPH_INTERVAL) {
        morphAutoTimer = 0;
        morphDemoViz.nextLayout();
      }
    }
  }

  // Feature 3: Live HR data — update every 2 seconds
  if (liveHREnabled && uiAnchor.visible) {
    hrUpdateTimer += dt;
    if (hrUpdateTimer > 2) {
      hrUpdateTimer = 0;
      doHRUpdate();
    }
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
  // FPS accounting + throttled repaint only — the panel is world-fixed,
  // not camera-tracked, so there's no per-frame repositioning.
  debugConsole?.tick(dt);

  // First valid XR frame after session start: re-anchor whatever UI the
  // user is looking at to the live XR camera. Runs once per session.
  if (pendingXrReplace && renderer.xr.isPresenting) {
    pendingXrReplace = false;
    const xrCam = renderer.xr.getCamera();
    if (_joinPanelRef?.visible()) {
      placeJoinPanelInFrontOfUser();
    } else {
      placeAnchorInFrontOfUser();
    }
    // Re-place the debug panel low + forward of the real XR head pose
    // (Spectacles' reference space differs from the desktop camera it
    // was first placed against).
    debugConsole?.place(xrCam);
    console.info('[xr] session started — re-placed active UI to XR camera');
  }

  if (!anchorPlaced && !renderer.xr.isPresenting) {
    uiAnchor.position.set(0, 1.3, 0);
    uiAnchor.rotation.set(0, 0, 0);
  }
  renderer.render(scene, camera);

  // 1 Hz draw-call / memory dump. Keep this format stable — it's the
  // ground truth for perf comparisons across UC1/2/3/4. `calls` is the
  // headline number Notblox cites (target < 200 for web). `triangles`
  // surfaces Gaussian-splat blowups (each splat in mkkellogg's mesh is
  // 2 tris; an 18 MB splat ≈ ~300k tris). `textures` + `geometries`
  // catch leaks across mode swaps.
  if (perfEnabled && time - lastPerfLog > 1000) {
    lastPerfLog = time;
    const r = renderer.info.render;
    const m = renderer.info.memory;
    console.log(
      `[perf] calls=${r.calls} tris=${r.triangles} ` +
      `points=${r.points} lines=${r.lines} | ` +
      `tex=${m.textures} geom=${m.geometries}`,
    );
  }
  stats?.end();
});
