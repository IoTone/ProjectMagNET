/**
 * renderManifest — P1.2
 *
 * Takes a LoadResult from loadManifest() and builds a gallery scene,
 * positioning each loaded mark in a grid layout with titles/subtitles.
 * Same layout logic as vizGallery.ts but driven by manifest data.
 */
import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';
export function renderManifestToScene(loadResult, parent, interact, nodeHoverFxs) {
    const root = new THREE.Group();
    root.name = `manifest-${loadResult.name}`;
    const marks = loadResult.marks;
    if (marks.length === 0) {
        parent.add(root);
        return { root, marks };
    }
    const cols = Math.min(4, marks.length);
    const rows = Math.ceil(marks.length / cols);
    const cellW = 0.38;
    const cellH = 0.30;
    const rowGap = 0.10;
    marks.forEach((mark, i) => {
        const col = i % cols;
        const row = Math.floor(i / cols);
        const x = (col - (cols - 1) / 2) * cellW;
        const y = ((rows - 1) / 2 - row) * (cellH + rowGap);
        const cell = new THREE.Group();
        cell.name = `cell-${mark.id}`;
        cell.position.set(x, y, 0);
        cell.add(mark.group);
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
            const viz = mark.viz;
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
                            if (fx)
                                fx.show(tmpVec, viz.getNodeLabel(ctx.instanceId));
                        }
                    },
                    onHoverOut: (ctx) => {
                        const hi = ctx?.handIndex ?? 2;
                        const fx = nodeHoverFxs[hi] ?? nodeHoverFxs[2];
                        if (fx)
                            fx.hide();
                    },
                    onHoverInstance: (instanceId, handIndex) => {
                        const hi = handIndex ?? 2;
                        const fx = nodeHoverFxs[hi] ?? nodeHoverFxs[2];
                        if (!fx)
                            return;
                        if (instanceId === null) {
                            fx.hide();
                            return;
                        }
                        if (viz.getNodeWorldPosition && viz.getNodeLabel) {
                            viz.getNodeWorldPosition(instanceId, tmpVec);
                            fx.show(tmpVec, viz.getNodeLabel(instanceId));
                        }
                    },
                });
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
                            if (fx)
                                fx.show(tmpVec, viz.getSegmentLabel(segIdx));
                        }
                    },
                    onHoverOut: (ctx) => {
                        const fx = nodeHoverFxs[ctx?.handIndex ?? 2] ?? nodeHoverFxs[2];
                        if (fx)
                            fx.hide();
                    },
                });
            }
        }
    });
    parent.add(root);
    return { root, marks };
}
