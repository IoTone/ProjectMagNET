import * as THREE from 'three';
import { sankey as d3Sankey, sankeyLeft } from 'd3-sankey';
const GROUP_COLORS = [0xff5577, 0x66ccff, 0xffcc66, 0xcc99ff];
export function buildSankey(data) {
    const group = new THREE.Group();
    group.name = 'sankey';
    const nodes = data.nodes.map(n => ({ ...n }));
    const links = data.links.map(l => ({ ...l }));
    const layout = d3Sankey()
        .nodeId((d) => d.id)
        .nodeWidth(0.015)
        .nodePadding(0.008)
        .nodeAlign(sankeyLeft)
        .extent([[0, 0], [0.3, 0.22]]);
    const graph = layout({
        nodes: nodes,
        links: links,
    });
    const layoutNodes = graph.nodes;
    const layoutLinks = graph.links;
    // Center the layout around origin
    const offsetX = -0.15;
    const offsetY = -0.11;
    // --- Node InstancedMesh (boxes) ---
    const boxGeo = new THREE.BoxGeometry(1, 1, 1);
    const boxMat = new THREE.MeshStandardMaterial({ roughness: 0.4, metalness: 0.1 });
    const nodeMesh = new THREE.InstancedMesh(boxGeo, boxMat, layoutNodes.length);
    nodeMesh.name = 'sankey-nodes';
    nodeMesh.userData.isSankeyNodes = true;
    const mat4 = new THREE.Matrix4();
    const colors = new Float32Array(layoutNodes.length * 3);
    layoutNodes.forEach((n, i) => {
        const x0 = (n.x0 ?? 0) + offsetX;
        const y0 = (n.y0 ?? 0) + offsetY;
        const x1 = (n.x1 ?? 0) + offsetX;
        const y1 = (n.y1 ?? 0) + offsetY;
        const w = x1 - x0;
        const h = y1 - y0;
        const depth = 0.008;
        mat4.compose(new THREE.Vector3(x0 + w / 2, y0 + h / 2, 0), new THREE.Quaternion(), new THREE.Vector3(w, h, depth));
        nodeMesh.setMatrixAt(i, mat4);
        const col = new THREE.Color(GROUP_COLORS[n.group % GROUP_COLORS.length]);
        colors[i * 3] = col.r;
        colors[i * 3 + 1] = col.g;
        colors[i * 3 + 2] = col.b;
    });
    nodeMesh.instanceColor = new THREE.InstancedBufferAttribute(colors, 3);
    nodeMesh.instanceMatrix.needsUpdate = true;
    group.add(nodeMesh);
    // --- Link tubes ---
    const maxValue = Math.max(...layoutLinks.map(l => l.value ?? 1));
    layoutLinks.forEach((l, linkIdx) => {
        const src = l.source;
        const tgt = l.target;
        const sx = (src.x1 ?? 0) + offsetX;
        const sy = (l.y0 ?? ((src.y0 ?? 0) + (src.y1 ?? 0)) / 2) + offsetY;
        const tx = (tgt.x0 ?? 0) + offsetX;
        const ty = (l.y1 ?? ((tgt.y0 ?? 0) + (tgt.y1 ?? 0)) / 2) + offsetY;
        // Arc slightly in +z for depth separation
        const zArc = 0.005 + (linkIdx / layoutLinks.length) * 0.015;
        const midX = (sx + tx) / 2;
        const midY = (sy + ty) / 2;
        const curve = new THREE.CatmullRomCurve3([
            new THREE.Vector3(sx, sy, 0),
            new THREE.Vector3(sx + (tx - sx) * 0.25, sy, zArc * 0.5),
            new THREE.Vector3(midX, midY, zArc),
            new THREE.Vector3(tx - (tx - sx) * 0.25, ty, zArc * 0.5),
            new THREE.Vector3(tx, ty, 0),
        ]);
        // Radius proportional to flow value
        const radius = 0.001 + ((l.value ?? 1) / maxValue) * 0.006;
        const tubeGeo = new THREE.TubeGeometry(curve, 24, radius, 6, false);
        const srcColor = new THREE.Color(GROUP_COLORS[src.group % GROUP_COLORS.length]);
        const tubeMat = new THREE.MeshStandardMaterial({
            color: srcColor,
            emissive: srcColor,
            emissiveIntensity: 0.35,
            transparent: true,
            opacity: 0.75,
            roughness: 0.5,
            metalness: 0.05,
        });
        const tubeMesh = new THREE.Mesh(tubeGeo, tubeMat);
        tubeMesh.userData.noHover = true;
        group.add(tubeMesh);
    });
    // --- Interface ---
    function getNodeWorldPosition(i, out = new THREE.Vector3()) {
        const n = layoutNodes[i];
        const x = ((n.x0 ?? 0) + (n.x1 ?? 0)) / 2 + offsetX;
        const y = ((n.y0 ?? 0) + (n.y1 ?? 0)) / 2 + offsetY;
        out.set(x, y, 0);
        group.localToWorld(out);
        return out;
    }
    return {
        group,
        nodeMesh,
        nodeCount: () => layoutNodes.length,
        getNodeWorldPosition,
        getNodeLabel: (i) => layoutNodes[i]?.name ?? '',
        getNodeInfo: (i) => {
            const n = layoutNodes[i];
            return { name: n.name, value: n.value ?? 0, group: n.group };
        },
    };
}
