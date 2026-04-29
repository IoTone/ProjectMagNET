import * as THREE from 'three';
import { hierarchy, pack as d3pack } from 'd3-hierarchy';
import { tweenMeshes, easeExpOut } from '../util/tween';
export function buildCircularPack(root, opts = {}) {
    const { diameter = 0.24, palette = [0xff5577, 0x66ccff, 0xffcc66, 0xcc99ff, 0x66ffcc], selectColor = 0x88ff99, } = opts;
    const g = new THREE.Group();
    g.name = 'circular-packing';
    const fullHier = hierarchy(root).sum(d => d.value ?? 1).sort((a, b) => (b.value - a.value));
    const allDescendants = fullHier.descendants();
    const focusPath = [];
    const spheres = [];
    const nodePositions = [];
    let packNodes = [];
    const sphereGeo = new THREE.SphereGeometry(1, 24, 24);
    function buildSpheres(subtreeRoot) {
        // Clear existing
        for (const m of spheres) {
            g.remove(m);
            m.material.dispose();
        }
        spheres.length = 0;
        nodePositions.length = 0;
        packNodes.length = 0;
        const hier = hierarchy(subtreeRoot).sum(d => d.value ?? 1).sort((a, b) => (b.value - a.value));
        d3pack().size([diameter, diameter]).padding(0.003)(hier);
        const desc = hier.descendants();
        desc.forEach((n, idx) => {
            const any = n;
            const isRoot = n.depth === 0;
            const isLeaf = !n.children;
            const mat = new THREE.MeshStandardMaterial({
                color: palette[n.depth % palette.length],
                emissive: palette[n.depth % palette.length],
                emissiveIntensity: isLeaf ? 0.45 : 0.12,
                roughness: 0.55,
                metalness: 0.05,
                transparent: true,
                opacity: isRoot ? 0.15 : (isLeaf ? 0.95 : 0.35),
                wireframe: isRoot,
            });
            const mesh = new THREE.Mesh(sphereGeo, mat);
            const pos = new THREE.Vector3(any.x - diameter / 2, diameter / 2 - any.y, 0);
            mesh.position.copy(pos);
            const r = any.r;
            mesh.scale.setScalar(r);
            mesh.userData.nodeIndex = spheres.length;
            g.add(mesh);
            nodePositions.push(pos.clone());
            packNodes.push(n);
            spheres.push(mesh);
        });
    }
    buildSpheres(root);
    // Selection markers
    const selectionMarkers = new Map();
    const markerMat = new THREE.MeshBasicMaterial({
        color: selectColor, transparent: true, opacity: 0.28,
        depthTest: false, side: THREE.BackSide,
    });
    let activeTween = null;
    function findAllDescIdx(node) {
        const path = [];
        let cur = node;
        while (cur) {
            path.unshift(cur.data.name);
            cur = cur.parent;
        }
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
    return {
        group: g,
        spheres,
        nodeCount: () => spheres.length,
        getNodeWorldPosition: (i, out = new THREE.Vector3()) => {
            const p = nodePositions[i];
            if (!p)
                return out.set(0, 0, 0);
            out.copy(p);
            g.localToWorld(out);
            return out;
        },
        getNodeLabel: (i) => packNodes[i]?.data.name ?? '',
        getNodeInfo: (i) => {
            const n = packNodes[i];
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
                const p = nodePositions[i];
                const mesh = spheres[i];
                if (!p || !mesh)
                    return false;
                const markerGeo = new THREE.SphereGeometry(1, 16, 16);
                const marker = new THREE.Mesh(markerGeo, markerMat);
                marker.position.copy(p);
                marker.scale.setScalar(mesh.scale.x * 1.5);
                marker.renderOrder = 993;
                marker.userData.noHover = true;
                g.add(marker);
                selectionMarkers.set(i, marker);
            }
            else {
                const marker = selectionMarkers.get(i);
                g.remove(marker);
                marker.geometry.dispose();
                selectionMarkers.delete(i);
            }
            return nowSel;
        },
        clearSelection: () => {
            for (const [, m] of selectionMarkers) {
                g.remove(m);
                m.geometry.dispose();
            }
            selectionMarkers.clear();
        },
        getSelected: () => new Set(selectionMarkers.keys()),
        drillIn: (nodeIndex) => {
            const node = packNodes[nodeIndex];
            if (!node || !node.children || node.children.length === 0)
                return false;
            const oldPositions = spheres.map(m => m.position.clone());
            const oldScales = spheres.map(m => m.scale.clone());
            const idx = findAllDescIdx(node);
            if (idx >= 0)
                focusPath.push(idx);
            for (const [, m] of selectionMarkers) {
                g.remove(m);
                m.geometry.dispose();
            }
            selectionMarkers.clear();
            buildSpheres(node.data);
            // Animate from center outward
            const from = spheres.map(() => ({
                pos: new THREE.Vector3(0, 0, 0),
                scale: new THREE.Vector3(0.01, 0.01, 0.01),
            }));
            const to = spheres.map((m, i) => ({
                pos: nodePositions[i]?.clone() ?? new THREE.Vector3(),
                scale: m.scale.clone(),
            }));
            spheres.forEach(m => { m.position.set(0, 0, 0); m.scale.setScalar(0.01); });
            activeTween = tweenMeshes(spheres, from, to, 500, easeExpOut);
            return true;
        },
        drillOut: () => {
            if (focusPath.length === 0)
                return false;
            focusPath.pop();
            for (const [, m] of selectionMarkers) {
                g.remove(m);
                m.geometry.dispose();
            }
            selectionMarkers.clear();
            if (focusPath.length === 0) {
                buildSpheres(root);
            }
            else {
                const parentNode = allDescendants[focusPath[focusPath.length - 1]];
                buildSpheres(parentNode.data);
            }
            const from = spheres.map(() => ({
                pos: new THREE.Vector3(0, 0, 0),
                scale: new THREE.Vector3(0.01, 0.01, 0.01),
            }));
            const to = spheres.map((m, i) => ({
                pos: nodePositions[i]?.clone() ?? new THREE.Vector3(),
                scale: m.scale.clone(),
            }));
            spheres.forEach(m => { m.position.set(0, 0, 0); m.scale.setScalar(0.01); });
            activeTween = tweenMeshes(spheres, from, to, 500, easeExpOut);
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
