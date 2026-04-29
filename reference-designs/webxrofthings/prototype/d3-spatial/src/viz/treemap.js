import * as THREE from 'three';
import { hierarchy, treemap as d3treemap } from 'd3-hierarchy';
import { tweenInstancedBoxes, easeExpOut } from '../util/tween';
export function buildTreemap(root, opts = {}) {
    const { size = [0.3, 0.22], maxExtrude = 0.05, palette = [0xff5577, 0x66ccff, 0xffcc66, 0xcc99ff, 0x66ffcc], selectColor = 0x88ff99, } = opts;
    const g = new THREE.Group();
    g.name = 'treemap';
    // Full hierarchy for drill navigation
    const fullHier = hierarchy(root).sum(d => d.value ?? 0).sort((a, b) => (b.value - a.value));
    const allDescendants = fullHier.descendants();
    // Current focus path (indices into allDescendants)
    const focusPath = [];
    // Compute layout for a given subtree root node
    function computeLayout(subtreeRoot) {
        // Re-run treemap on the subtree
        const subHier = hierarchy(subtreeRoot.data).sum(d => d.value ?? 0).sort((a, b) => (b.value - a.value));
        d3treemap().size(size).paddingInner(0.004)(subHier);
        return subHier.leaves();
    }
    // Initial layout
    let currentRoot = fullHier;
    d3treemap().size(size).paddingInner(0.004)(fullHier);
    let leaves = fullHier.leaves();
    // We need a max instance count — use the full leaf count
    const maxLeaves = fullHier.leaves().length;
    const boxGeo = new THREE.BoxGeometry(1, 1, 1);
    const meshes = new THREE.InstancedMesh(boxGeo, new THREE.MeshStandardMaterial({ roughness: 0.5, metalness: 0.05 }), maxLeaves);
    meshes.name = 'treemap-cells';
    // Cache per-leaf data
    let positions = [];
    let scales = [];
    let leafNodes = [];
    function rebuildFromLeaves(leavesArr) {
        leafNodes = leavesArr;
        positions = [];
        scales = [];
        const colors = new Float32Array(maxLeaves * 3);
        const m = new THREE.Matrix4();
        const maxV = currentRoot.value ?? 1;
        for (let i = 0; i < leavesArr.length; i++) {
            const leaf = leavesArr[i];
            const any = leaf;
            const w = any.x1 - any.x0;
            const h = any.y1 - any.y0;
            const v = leaf.value ?? 1;
            const ext = 0.005 + (v / maxV) * maxExtrude;
            const cx = any.x0 + w / 2 - size[0] / 2;
            const cy = size[1] / 2 - (any.y0 + h / 2);
            const pos = new THREE.Vector3(cx, cy, ext / 2);
            const scl = new THREE.Vector3(w * 0.94, h * 0.94, ext);
            positions.push(pos);
            scales.push(scl);
            m.compose(pos, new THREE.Quaternion(), scl);
            meshes.setMatrixAt(i, m);
            const col = new THREE.Color(palette[(leaf.parent?.data?.name?.length ?? 0) % palette.length] ?? palette[0]);
            colors[i * 3] = col.r;
            colors[i * 3 + 1] = col.g;
            colors[i * 3 + 2] = col.b;
        }
        // Hide unused instances
        for (let i = leavesArr.length; i < maxLeaves; i++) {
            m.compose(new THREE.Vector3(0, 0, -100), new THREE.Quaternion(), new THREE.Vector3(0, 0, 0));
            meshes.setMatrixAt(i, m);
            colors[i * 3] = 0;
            colors[i * 3 + 1] = 0;
            colors[i * 3 + 2] = 0;
        }
        meshes.instanceColor = new THREE.InstancedBufferAttribute(colors, 3);
        meshes.instanceMatrix.needsUpdate = true;
        meshes.count = leavesArr.length;
    }
    rebuildFromLeaves(leaves);
    g.add(meshes);
    // Selection markers
    const selectionMarkers = new Map();
    const markerGeo = new THREE.BoxGeometry(1, 1, 1);
    const markerMat = new THREE.MeshBasicMaterial({
        color: selectColor, transparent: true, opacity: 0.28,
        depthTest: false, side: THREE.BackSide,
    });
    // Tween state
    let activeTween = null;
    // Find descendant index in allDescendants by matching node data name + depth
    function findDescendantIndex(node) {
        // Walk up to build the path from root
        const path = [];
        let cur = node;
        while (cur) {
            path.unshift(cur.data.name);
            cur = cur.parent;
        }
        // Find matching node in allDescendants by path
        for (let i = 0; i < allDescendants.length; i++) {
            const n = allDescendants[i];
            const nPath = [];
            let c = n;
            while (c) {
                nPath.unshift(c.data.name);
                c = c.parent;
            }
            if (nPath.length === path.length && nPath.every((v, j) => v === path[j]))
                return i;
        }
        return -1;
    }
    // Find the parent node in the current layout that owns leaf i
    function findParentOfLeaf(leafIdx) {
        const leaf = leafNodes[leafIdx];
        if (!leaf)
            return null;
        return leaf.parent;
    }
    return {
        group: g,
        nodeMesh: meshes,
        nodeCount: () => leafNodes.length,
        getNodeWorldPosition: (i, out = new THREE.Vector3()) => {
            const p = positions[i];
            if (!p)
                return out.set(0, 0, 0);
            out.copy(p);
            g.localToWorld(out);
            return out;
        },
        getNodeLabel: (i) => leafNodes[i]?.data.name ?? '',
        getNodeInfo: (i) => {
            const n = leafNodes[i];
            return {
                name: n?.data.name ?? '',
                depth: n?.depth ?? 0,
                childCount: n?.children?.length ?? 0,
                isLeaf: !n?.children,
                value: n?.value,
            };
        },
        toggleSelected: (i) => {
            const nowSel = !selectionMarkers.has(i);
            if (nowSel) {
                const marker = new THREE.Mesh(markerGeo, markerMat);
                const p = positions[i];
                const s = scales[i];
                if (p && s) {
                    marker.position.copy(p);
                    marker.scale.set(s.x * 1.15, s.y * 1.15, s.z * 1.8);
                }
                marker.renderOrder = 993;
                marker.userData.noHover = true;
                g.add(marker);
                selectionMarkers.set(i, marker);
            }
            else {
                const marker = selectionMarkers.get(i);
                g.remove(marker);
                selectionMarkers.delete(i);
            }
            return nowSel;
        },
        clearSelection: () => {
            for (const [, m] of selectionMarkers)
                g.remove(m);
            selectionMarkers.clear();
        },
        getSelected: () => new Set(selectionMarkers.keys()),
        drillIn: (nodeIndex) => {
            // nodeIndex is a leaf index; find its parent to drill into
            const parent = findParentOfLeaf(nodeIndex);
            if (!parent || !parent.children || parent.children.length === 0)
                return false;
            // Save old positions for tween
            const oldPositions = positions.map(p => p.clone());
            const oldScales = scales.map(s => s.clone());
            // Record focus
            const parentIdx = findDescendantIndex(parent);
            if (parentIdx >= 0)
                focusPath.push(parentIdx);
            // Re-layout from the parent subtree
            const newLeaves = computeLayout(parent);
            currentRoot = parent;
            rebuildFromLeaves(newLeaves);
            // Clear selection on drill
            for (const [, m] of selectionMarkers)
                g.remove(m);
            selectionMarkers.clear();
            // Setup tween from old to new
            const fromArr = oldPositions.map((p, i) => ({
                pos: p,
                scale: oldScales[i] ?? new THREE.Vector3(0, 0, 0),
            }));
            const toArr = positions.map((p, i) => ({
                pos: p,
                scale: scales[i] ?? new THREE.Vector3(0, 0, 0),
            }));
            // Pad arrays to match
            const tweenCount = Math.max(fromArr.length, toArr.length);
            while (fromArr.length < tweenCount)
                fromArr.push({ pos: new THREE.Vector3(0, 0, -1), scale: new THREE.Vector3(0, 0, 0) });
            while (toArr.length < tweenCount)
                toArr.push({ pos: new THREE.Vector3(0, 0, -1), scale: new THREE.Vector3(0, 0, 0) });
            meshes.count = tweenCount;
            activeTween = tweenInstancedBoxes(meshes, fromArr, toArr, 500, easeExpOut);
            return true;
        },
        drillOut: () => {
            if (focusPath.length === 0)
                return false;
            focusPath.pop();
            const oldPositions = positions.map(p => p.clone());
            const oldScales = scales.map(s => s.clone());
            // Find new root
            let newRoot;
            if (focusPath.length === 0) {
                // Back to full tree
                const h = hierarchy(root).sum(d => d.value ?? 0).sort((a, b) => (b.value - a.value));
                d3treemap().size(size).paddingInner(0.004)(h);
                newRoot = h;
                const newLeaves = newRoot.leaves();
                currentRoot = newRoot;
                rebuildFromLeaves(newLeaves);
            }
            else {
                const parentNode = allDescendants[focusPath[focusPath.length - 1]];
                const newLeaves = computeLayout(parentNode);
                currentRoot = parentNode;
                rebuildFromLeaves(newLeaves);
            }
            for (const [, m] of selectionMarkers)
                g.remove(m);
            selectionMarkers.clear();
            const fromArr = oldPositions.map((p, i) => ({
                pos: p,
                scale: oldScales[i] ?? new THREE.Vector3(0, 0, 0),
            }));
            const toArr = positions.map((p, i) => ({
                pos: p,
                scale: scales[i] ?? new THREE.Vector3(0, 0, 0),
            }));
            const tweenCount = Math.max(fromArr.length, toArr.length);
            while (fromArr.length < tweenCount)
                fromArr.push({ pos: new THREE.Vector3(0, 0, -1), scale: new THREE.Vector3(0, 0, 0) });
            while (toArr.length < tweenCount)
                toArr.push({ pos: new THREE.Vector3(0, 0, -1), scale: new THREE.Vector3(0, 0, 0) });
            meshes.count = tweenCount;
            activeTween = tweenInstancedBoxes(meshes, fromArr, toArr, 500, easeExpOut);
            return true;
        },
        getFocusPath: () => [...focusPath],
        getFocusLabels: () => {
            const labels = ['root'];
            for (const idx of focusPath) {
                const n = allDescendants[idx];
                if (n)
                    labels.push(n.data.name);
            }
            return labels;
        },
        tick: () => {
            if (activeTween && !activeTween.done) {
                activeTween.tick();
                if (activeTween.done)
                    activeTween = null;
            }
        },
    };
}
