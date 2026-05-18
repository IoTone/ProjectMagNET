/**
 * renderManifest — P1.2
 *
 * Takes a LoadResult from loadManifest() and builds a gallery scene,
 * positioning each loaded mark in a grid layout with titles/subtitles.
 * Same layout logic as vizGallery.ts but driven by manifest data.
 */

import * as THREE from 'three';
import { Text } from 'troika-three-text';
import type { LoadResult, LoadedMark } from './loader';
import type { Interact } from '../interact/Interact';
import type { NodeHoverFx } from '../ui/NodeHoverFx';
import { TEXT } from '../ui/palette';

export interface ManifestSceneResult {
  root: THREE.Group;
  marks: LoadedMark[];
  /**
   * Tear the rendered scene down: detach `root` from its parent and
   * unregister every Interact item this render added. MUST be called
   * before rendering a replacement manifest and on leave-dataspace —
   * without it the old root + its interactables linger, so a
   * leave→rejoin stacked a second copy of every mark ("Room controls"
   * twice, content piled on the controls) and broke rejoin.
   */
  dispose(): void;
}

export function renderManifestToScene(
  loadResult: LoadResult,
  parent: THREE.Group,
  interact: Interact,
  nodeHoverFxs: NodeHoverFx[],
): ManifestSceneResult {
  const root = new THREE.Group();
  root.name = `manifest-${loadResult.name}`;

  // Every Interact id this render registers — collected so dispose()
  // can unregister exactly these (and nothing else) on teardown.
  const registeredIds: string[] = [];
  const reg = (h: Parameters<Interact['add']>[0]) => {
    registeredIds.push(h.id);
    interact.add(h);
  };
  const makeDispose = () => () => {
    if (root.parent) root.parent.remove(root);
    for (const id of registeredIds) interact.remove(id);
    registeredIds.length = 0;
  };

  const marks = loadResult.marks;
  if (marks.length === 0) {
    parent.add(root);
    return { root, marks, dispose: makeDispose() };
  }

  // "Stage mode" — when any mark is authored as defaultVisible:false the
  // manifest is using show-only HUD switching (UC4-style: mutually
  // exclusive content modes). In that case the horizontal-grid layout
  // makes no sense — only one cell is ever visible, and the user wants
  // it centred on the viewer's gaze. Stack every cell at the origin so
  // whichever one is currently visible appears in the middle of the
  // dataspace. Gallery-style manifests (all marks visible at once)
  // keep the grid layout.
  const stageMode = marks.some(m => m.defaultVisible === false);

  // Actuator panels are large (~0.70 tall) and own their own chrome —
  // they don't belong in the small sensor grid (they swallowed the
  // neighbouring cells, the "collapsed on top of the controls" report).
  // Lay the sensor/data marks out in the grid, then drop any
  // actuator-panel mark into a dedicated band BELOW the grid so the
  // simulated data sits above the controls in Y, as requested.
  const gridMarks = marks.filter(m => m.type !== 'actuator-panel');

  const cellW = 0.38;
  const cellH = 0.30;
  const rowGap = 0.10;
  const rowPitch = cellH + rowGap;
  const cols = stageMode ? 1 : Math.min(4, Math.max(1, gridMarks.length));
  const rows = stageMode ? 1 : Math.ceil(Math.max(1, gridMarks.length) / cols);

  // Actuator panel placement: to the LEFT of the sensor grid, angled
  // ~22° inward to face the user. Previously it sat *below* the grid
  // which (a) forced a big grid lift that pushed everything up under
  // the title, and (b) parked it right where the camera-locked debug
  // panel lives. Side placement keeps the simulated data grid centred
  // (no lift) and clear of the debug panel.
  const PANEL_HALF_W = 0.34;          // ~half the actuator panel's width + pad
  const SIDE_GAP = 0.06;
  const ACTUATOR_ANGLE = THREE.MathUtils.degToRad(22);   // toe-in toward the user
  const gridLeftEdge = (0 - (cols - 1) / 2) * cellW - cellW / 2;
  const panelX = gridLeftEdge - SIDE_GAP - PANEL_HALF_W;

  marks.forEach((mark) => {
    const isPanel = mark.type === 'actuator-panel';
    let x: number, y: number;
    if (stageMode) {
      x = 0; y = 0;
    } else if (isPanel) {
      x = panelX; y = 0;            // left of the grid, vertically centred
    } else {
      const gi = gridMarks.indexOf(mark);
      const col = gi % cols;
      const row = Math.floor(gi / cols);
      x = (col - (cols - 1) / 2) * cellW;
      y = ((rows - 1) / 2 - row) * rowPitch;   // centred grid — no lift
    }

    const cell = new THREE.Group();
    cell.name = `cell-${mark.id}`;
    cell.position.set(x, y, 0);
    // Toe the side-mounted actuator panel in toward the viewer so it
    // isn't edge-on. Sign chosen so a left-of-centre panel rotates its
    // face back toward a centred user; flip the sign if it reads wrong.
    if (isPanel && !stageMode) cell.rotation.y = ACTUATOR_ANGLE;
    cell.add(mark.group);

    // Manifest can author marks that start hidden — used by UC4 to switch
    // between mutually-exclusive content modes via `show-only:<id>` HUD
    // actions. The cell stays in the scene graph so the HUD handler can
    // flip visibility back on without reloading data.
    if (mark.defaultVisible === false) cell.visible = false;

    // Title / subtitle — skipped for actuator-panel: that cell draws its
    // own title at the correct spot for its tall panel, and the generic
    // one (placed at cellH/2 above centre) would float mid-panel and
    // read as a duplicate ("Room controls" twice).
    if (!isPanel) {
      const title = new Text();
      title.text = mark.title;
      title.fontSize = 0.018;
      title.color = TEXT.primary;
      title.anchorX = 'center';
      title.anchorY = 'bottom';
      title.position.set(0, cellH / 2 + 0.015, 0.03);
      title.sync();
      cell.add(title);

      if (mark.subtitle) {
        const sub = new Text();
        sub.text = mark.subtitle;
        sub.fontSize = 0.011;
        sub.color = TEXT.muted;
        sub.anchorX = 'center';
        sub.anchorY = 'top';
        sub.position.set(0, -cellH / 2 - 0.015, 0.03);
        sub.sync();
        cell.add(sub);
      }
    }

    root.add(cell);

    // Register hoverable marks with Interact
    if (mark.hoverable && mark.viz) {
      const viz = mark.viz as any;
      const tmpVec = new THREE.Vector3();

      // Force graphs have nodeMesh
      if (viz.nodeMesh) {
        reg({
          id: `manifest:${mark.id}:nodes`,
          object: viz.nodeMesh,
          supportsInstances: true,
          onHoverIn: (ctx) => {
            const hi = ctx?.handIndex ?? 2;
            if (ctx?.instanceId !== undefined && viz.getNodeWorldPosition && viz.getNodeLabel) {
              viz.getNodeWorldPosition(ctx.instanceId, tmpVec);
              const fx = nodeHoverFxs[hi] ?? nodeHoverFxs[2];
              if (fx) fx.show(tmpVec, viz.getNodeLabel(ctx.instanceId));
            }
          },
          onHoverOut: (ctx) => {
            const hi = ctx?.handIndex ?? 2;
            const fx = nodeHoverFxs[hi] ?? nodeHoverFxs[2];
            if (fx) fx.hide();
          },
          onHoverInstance: (instanceId: number | null, handIndex?: number) => {
            const hi = handIndex ?? 2;
            const fx = nodeHoverFxs[hi] ?? nodeHoverFxs[2];
            if (!fx) return;
            if (instanceId === null) { fx.hide(); return; }
            if (viz.getNodeWorldPosition && viz.getNodeLabel) {
              viz.getNodeWorldPosition(instanceId, tmpVec);
              fx.show(tmpVec, viz.getNodeLabel(instanceId));
            }
          },
        });
      }

      // Generic click handler for marks that expose a `clickTarget` Object3D
      // and an `onSelect()` method. Used by the spatial-audio cell to let
      // the user tap the boombox to cycle music themes; any future cell
      // that wants the same affordance just needs to expose the same two
      // members. Registered before the segment-hover path so clicks on
      // clickable cells don't fall through to a stray hover handler. The
      // hover callbacks are no-ops — the Hoverable interface requires
      // them but this cell doesn't currently surface hover feedback.
      if (viz.clickTarget && typeof viz.onSelect === 'function') {
        reg({
          id: `manifest:${mark.id}:select`,
          object: viz.clickTarget,
          onHoverIn:  () => {},
          onHoverOut: () => {},
          onSelect:   () => viz.onSelect(),
        });
      }

      // Multi-control cells (the UC2 actuator panel) expose
      // getInteractables() — the same shape the join panel uses: one
      // entry per pressable block, with optional rich hover hooks.
      // Register each with Interact, namespacing the id by mark so two
      // panels can't collide. Falls back to a plain opacity bump for
      // entries that don't supply their own hover feedback.
      if (typeof viz.getInteractables === 'function') {
        for (const e of viz.getInteractables() as Array<{
          id: string;
          object: THREE.Object3D;
          onSelect: () => void;
          onHoverIn?: () => void;
          onHoverOut?: () => void;
        }>) {
          reg({
            id: `manifest:${mark.id}:${e.id}`,
            object: e.object,
            onHoverIn: e.onHoverIn ?? (() => { (e.object as any).set?.({ backgroundOpacity: 1.0 }); }),
            onHoverOut: e.onHoverOut ?? (() => { (e.object as any).set?.({ backgroundOpacity: 0.9 }); }),
            onSelect: () => e.onSelect(),
          });
        }
      }

      // Hierarchy marks with group-level hover (sunburst, pack)
      if (viz.group && viz.getSegmentWorldPosition) {
        reg({
          id: `manifest:${mark.id}:segments`,
          object: viz.group,
          onHoverIn: (ctx) => {
            const segIdx = ctx?.hitObject?.userData?.segmentIndex;
            if (segIdx !== undefined && viz.getSegmentWorldPosition && viz.getSegmentLabel) {
              viz.getSegmentWorldPosition(segIdx, tmpVec);
              const fx = nodeHoverFxs[ctx?.handIndex ?? 2] ?? nodeHoverFxs[2];
              if (fx) fx.show(tmpVec, viz.getSegmentLabel(segIdx));
            }
          },
          onHoverOut: (ctx) => {
            const fx = nodeHoverFxs[ctx?.handIndex ?? 2] ?? nodeHoverFxs[2];
            if (fx) fx.hide();
          },
        });
      }
    }
  });

  parent.add(root);
  return { root, marks, dispose: makeDispose() };
}
