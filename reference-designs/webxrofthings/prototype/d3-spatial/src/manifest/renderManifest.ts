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
import { buildOfflineSensorsHud } from './offlineSensorsHud';
import { buildDemoModeHud }       from './demoModeHud';

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
  /* Collected per-frame disposables (HUD widgets — see end of this
   * function). makeDispose runs them in dispose() so the offline-
   * sensors + DEMO HUDs free their geometry/material + unsubscribe
   * their health-monitor listener on dataspace teardown. */
  const cleanupTasks: Array<() => void> = [];
  const makeDispose = () => () => {
    if (root.parent) root.parent.remove(root);
    for (const id of registeredIds) interact.remove(id);
    registeredIds.length = 0;
    for (const t of cleanupTasks) { try { t(); } catch { /* swallow */ } }
    cleanupTasks.length = 0;
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
  //
  // SELF_POSITIONED marks aren't grid-placeable at all — they author
  // their own world placement (moon-phases-arc wraps the user from in
  // front around to behind; can't be a 0.38 × 0.30 grid cell). They
  // bypass both the grid loop and the per-cell wrapping group below.
  const SELF_POSITIONED = new Set(['moon-phases-arc', 'owls-to-the-max', 'force-tree-3d']);
  const gridMarks = marks.filter(m =>
    m.type !== 'actuator-panel' && !SELF_POSITIONED.has(m.type),
  );

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
    const isPanel        = mark.type === 'actuator-panel';
    const isSelfPos      = SELF_POSITIONED.has(mark.type);

    /* Self-positioned marks (moon-phases-arc / owls-to-the-max /
     * force-tree-3d): attach the group directly to root with no cell
     * wrapper, no title overlay, no positioning — the viz authors its
     * own world placement. Note: we DO NOT early-return — the hoverable
     * / getInteractables / clickTarget registration block further down
     * applies just the same. A previous version `return`ed here, which
     * silently dropped Interact registration for self-positioned marks
     * (the tree's getInteractables was never wired → beam hit the orbs
     * via the reticle raycast but Interact saw no hit, so hover/click
     * were dead). */
    if (isSelfPos) {
      root.add(mark.group);
      if (mark.defaultVisible === false) mark.group.visible = false;
    }

    /* The cell-wrapping block below only applies to grid-placed marks.
     * Self-positioned marks already attached their group to root above;
     * if we ran through the cell-wrap path here too, mark.group would
     * be re-parented into a positioned cell and end up at the wrong
     * world location. Skip placement/title for those — but still fall
     * through to the Interact registration block past the wrap. */
    if (!isSelfPos) {
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
        /* Use the mark's actual bounding box (not the fixed cellH/2) so
         * a mark that's authored taller than the standard 0.30 m cell —
         * voronoi-stippling at 0.64 m, the line charts at 0.16 m, etc. —
         * gets its title above its own top edge, not mid-panel.
         * Falls back to the legacy half-cell offset when the bbox is
         * empty (no geometry yet — sankey/streamgraph build asynchronously). */
        const bbox  = new THREE.Box3().setFromObject(mark.group);
        const valid = isFinite(bbox.max.y) && bbox.max.y > -Infinity;
        const topY  = valid ? bbox.max.y : cellH / 2;
        const botY  = valid ? bbox.min.y : -cellH / 2;
        const title = new Text();
        title.text = mark.title;
        title.fontSize = 0.018;
        title.color = TEXT.primary;
        title.anchorX = 'center';
        title.anchorY = 'bottom';
        title.position.set(0, topY + 0.015, 0.03);
        title.sync();
        cell.add(title);

        if (mark.subtitle) {
          const sub = new Text();
          sub.text = mark.subtitle;
          sub.fontSize = 0.011;
          sub.color = TEXT.muted;
          sub.anchorX = 'center';
          sub.anchorY = 'top';
          sub.position.set(0, botY - 0.015, 0.03);
          sub.sync();
          cell.add(sub);
        }
      }

      root.add(cell);
    }

    // Register hoverable marks with Interact
    if (mark.hoverable && mark.viz) {
      const viz = mark.viz as any;
      const tmpVec = new THREE.Vector3();

      // Force graphs have nodeMesh
      if (viz.nodeMesh) {
        /* Drag-state local to this Interact item. Per-hand because XR
         * controllers + hand tracking can both have a grab in flight
         * simultaneously (left + right). Indices 0/1 are XR hands;
         * 2 is the mouse fallback. */
        const dragNodeIds: (number | null)[] = [null, null, null];
        /* Only register drag callbacks when the viz actually supports
         * grab-and-move (pinNode/unpinNode). Other "viz.nodeMesh"
         * marks — sankey, treemap, etc. — don't need this, and giving
         * Interact a no-op onDragStart would still consume the
         * controller-trigger and break their click handlers. */
        const supportsGrab =
          typeof viz.pinNode === 'function' && typeof viz.unpinNode === 'function';
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
          ...(supportsGrab ? {
            onDragStart: (ctx: { instanceId?: number; handIndex?: number }) => {
              if (ctx.instanceId === undefined) return false;
              const hi = ctx.handIndex ?? 2;
              dragNodeIds[hi] = ctx.instanceId;
              viz.reheat?.(0.5);
              return true;
            },
            onDragMove: (worldPoint: THREE.Vector3, handIndex?: number) => {
              const hi = handIndex ?? 2;
              const nodeId = dragNodeIds[hi];
              if (nodeId !== null && nodeId !== undefined) {
                viz.pinNode(nodeId, worldPoint);
                const fx = nodeHoverFxs[hi] ?? nodeHoverFxs[2];
                if (fx) fx.updatePosition(worldPoint);
              }
            },
            onDragEnd: (ctx?: { handIndex?: number }) => {
              const hi = ctx?.handIndex ?? 2;
              const nodeId = dragNodeIds[hi];
              if (nodeId !== null && nodeId !== undefined) viz.unpinNode(nodeId);
              dragNodeIds[hi] = null;
            },
          } : {}),
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

  /* ─── Offline-sensors + DEMO HUDs ───────────────────────────────
   *
   * Both HUDs hang off `root` so they share the dataspace's lookAt
   * orientation but ride above the cell-grid top edge. Positioned
   * top-center, side-by-side: offline-card on the left, DEMO badge
   * on the right.
   *
   * The cell grid's top edge sits at +((rows-1)/2)*rowPitch + cellH/2.
   * For UC1 (5 marks → 4 cols × 2 rows, rowPitch ≈ 0.40, cellH ≈ 0.30)
   * that's ~+0.35. Add a 0.05 m chrome gap and another half the HUD
   * card height; ~+0.48 lands the cards in clear space between the
   * marks and the dataspace title (which sits at +0.32 in cell coords
   * — set by setDataspaceTitle in main.ts).
   *
   * The offline-sensors card stays hidden until any mark goes offline;
   * the DEMO badge mirrors visibility. So a healthy dataspace shows
   * neither and the chrome is invisible. */
  const offlineHud = buildOfflineSensorsHud();
  const demoHud    = buildDemoModeHud();
  offlineHud.group.position.set(-0.20, 0.55, 0.01);
  demoHud.group.position.set    ( 0.20, 0.55, 0.01);
  root.add(offlineHud.group);
  root.add(demoHud.group);
  const unsubHealth = loadResult.health.onChange((offlineIds) => {
    offlineHud.setOfflineIds(offlineIds);
    demoHud.setVisible(offlineIds.length > 0);
    if (offlineIds.length > 0) {
      const total = marks.length;
      demoHud.setLabel(offlineIds.length === total
        ? 'DEMO MODE'
        : `DEMO MODE · ${offlineIds.length}/${total}`);
    }
  });
  /* Apply the initial state — if any mark already failed during the
   * loader's pre-fetch phase, the HUDs need to reflect it on the
   * very first render rather than waiting for the next health event. */
  const initialOffline = loadResult.health.offlineMarkIds();
  if (initialOffline.length > 0) {
    offlineHud.setOfflineIds(initialOffline);
    demoHud.setVisible(true);
    const total = marks.length;
    demoHud.setLabel(initialOffline.length === total
      ? 'DEMO MODE'
      : `DEMO MODE · ${initialOffline.length}/${total}`);
  }
  cleanupTasks.push(() => { unsubHealth(); offlineHud.dispose(); demoHud.dispose(); });

  /* Self-fetching cells (UC4 imu, UC2 video) bypass the loader's
   * URL-fetch + recordFetch path and run their own polling
   * internally. Mirror their state into the global health monitor
   * so the offline-sensors HUD + DEMO badge cover them too.
   *
   * Cells can additionally expose `isAutonomous()` (UC4 imu) — when
   * true, the cell has been failing for long enough that it's now
   * painting synthesised data. THAT is the trigger for the DEMO HUD,
   * not raw `state==='offline'` (which fires within a second of a
   * transient outage and would flicker the HUD on every blip).
   * Cells that don't expose isAutonomous fall back to the raw state. */
  const selfPolling = marks.filter(m => {
    const v = m.viz as { getStatus?: () => { state: 'live' | 'stale' | 'offline' } } | null;
    return typeof v?.getStatus === 'function';
  });
  if (selfPolling.length > 0) {
    const handle = setInterval(() => {
      for (const m of selfPolling) {
        const v = m.viz as {
          getStatus: () => { state: 'live' | 'stale' | 'offline' };
          isAutonomous?: () => boolean;
        };
        const rawState   = v.getStatus().state;
        const autonomous = v.isAutonomous?.() ?? false;
        if (autonomous) {
          loadResult.health.forceState(m.id, 'offline');
        } else if (rawState === 'live') {
          loadResult.health.forceState(m.id, 'live');
        }
        /* Else (cell offline but pre-autonomous): leave manifest
         * state alone — the user shouldn't see DEMO yet. */
      }
    }, 1000);
    cleanupTasks.push(() => clearInterval(handle));
  }

  parent.add(root);
  return { root, marks, dispose: makeDispose() };
}
