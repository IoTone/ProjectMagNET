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
}

export function renderManifestToScene(
  loadResult: LoadResult,
  parent: THREE.Group,
  interact: Interact,
  nodeHoverFxs: NodeHoverFx[],
): ManifestSceneResult {
  const root = new THREE.Group();
  root.name = `manifest-${loadResult.name}`;

  const marks = loadResult.marks;
  if (marks.length === 0) {
    parent.add(root);
    return { root, marks };
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

  const cols = stageMode ? 1 : Math.min(4, marks.length);
  const rows = stageMode ? 1 : Math.ceil(marks.length / cols);
  const cellW = 0.38;
  const cellH = 0.30;
  const rowGap = 0.10;

  marks.forEach((mark, i) => {
    const col = i % cols;
    const row = Math.floor(i / cols);
    const x = stageMode ? 0 : (col - (cols - 1) / 2) * cellW;
    const y = stageMode ? 0 : ((rows - 1) / 2 - row) * (cellH + rowGap);

    const cell = new THREE.Group();
    cell.name = `cell-${mark.id}`;
    cell.position.set(x, y, 0);
    cell.add(mark.group);

    // Manifest can author marks that start hidden — used by UC4 to switch
    // between mutually-exclusive content modes via `show-only:<id>` HUD
    // actions. The cell stays in the scene graph so the HUD handler can
    // flip visibility back on without reloading data.
    if (mark.defaultVisible === false) cell.visible = false;

    // Title label
    const title = new Text();
    title.text = mark.title;
    title.fontSize = 0.018;
    title.color = TEXT.primary;
    title.anchorX = 'center';
    title.anchorY = 'bottom';
    title.position.set(0, cellH / 2 + 0.015, 0.03);
    title.sync();
    cell.add(title);

    // Subtitle label
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

    root.add(cell);

    // Register hoverable marks with Interact
    if (mark.hoverable && mark.viz) {
      const viz = mark.viz as any;
      const tmpVec = new THREE.Vector3();

      // Force graphs have nodeMesh
      if (viz.nodeMesh) {
        interact.add({
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
        interact.add({
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
          interact.add({
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
        interact.add({
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
  return { root, marks };
}
