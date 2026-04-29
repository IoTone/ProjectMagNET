import * as THREE from 'three';
import { hierarchy, cluster, treemap as d3treemap, pack as d3pack } from 'd3-hierarchy';
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';
import { tweenInstanced, easeExpOut } from '../util/tween';
const LAYOUT_CYCLE = ['tree', 'sunburst', 'treemap', 'pack'];
const LAYOUT_LABELS = {
    tree: 'tree \u00b7 radial',
    sunburst: 'sunburst \u00b7 stacked',
    treemap: 'treemap \u00b7 extruded',
    pack: 'pack \u00b7 nested',
};
export function buildMorphDemo(rootData) {
    const g = new THREE.Group();
    g.name = 'morph-demo';
    const hier = hierarchy(rootData).sum(d => d.value ?? 1).sort((a, b) => (b.value - a.value));
    const allNodes = hier.descendants();
    const nodeCount = allNodes.length;
    // Single InstancedMesh for all nodes
    const nodeGeo = new THREE.SphereGeometry(1, 14, 14);
    const nodeMat = new THREE.MeshStandardMaterial({
        color: 0x66ccff, emissive: 0x66ccff, emissiveIntensity: 0.35,
        roughness: 0.4, metalness: 0.1,
    });
    const mesh = new THREE.InstancedMesh(nodeGeo, nodeMat, nodeCount);
    mesh.name = 'morph-nodes';
    // Per-instance colors
    const palette = [0xff5577, 0x66ccff, 0xffcc66, 0xcc99ff, 0x66ffcc];
    const colors = new Float32Array(nodeCount * 3);
    allNodes.forEach((n, i) => {
        const col = new THREE.Color(palette[n.depth % palette.length]);
        colors[i * 3] = col.r;
        colors[i * 3 + 1] = col.g;
        colors[i * 3 + 2] = col.b;
    });
    mesh.instanceColor = new THREE.InstancedBufferAttribute(colors, 3);
    g.add(mesh);
    // Edges
    const edgeMat = new THREE.LineBasicMaterial({ color: 0xb8a380, transparent: true, opacity: 0.6 });
    let edgeGeo = new THREE.BufferGeometry();
    let edgeLines = new THREE.LineSegments(edgeGeo, edgeMat);
    edgeLines.userData.noHover = true;
    g.add(edgeLines);
    // Layout type label
    const label = new Text();
    label.fontSize = 0.014;
    label.color = TEXT.primary;
    label.anchorX = 'center';
    label.anchorY = 'bottom';
    label.position.set(0, 0.17, 0.01);
    label.text = LAYOUT_LABELS['tree'];
    label.sync();
    g.add(label);
    let currentIdx = 0;
    let currentPositions = [];
    let activeTween = null;
    const SIZE = 0.28;
    const RADIUS = 0.12;
    function computeLayout(type) {
        // Re-create hierarchy for each layout since d3 mutates nodes
        const h = hierarchy(rootData).sum(d => d.value ?? 1).sort((a, b) => (b.value - a.value));
        const desc = h.descendants();
        const result = [];
        switch (type) {
            case 'tree': {
                const lay = cluster().size([2 * Math.PI, RADIUS]);
                lay(h);
                for (const n of desc) {
                    const theta = n.x;
                    const r = n.y;
                    result.push({
                        pos: new THREE.Vector3(Math.cos(theta - Math.PI / 2) * r, Math.sin(theta - Math.PI / 2) * r, 0),
                        scale: n.children ? 0.006 : 0.005,
                    });
                }
                break;
            }
            case 'sunburst': {
                // Use partition-like positioning: arrange on rings
                const maxDepth = Math.max(...desc.map(n => n.depth));
                // Group nodes by depth and distribute angularly
                const byDepth = new Map();
                for (const n of desc) {
                    if (!byDepth.has(n.depth))
                        byDepth.set(n.depth, []);
                    byDepth.get(n.depth).push(n);
                }
                for (const n of desc) {
                    const siblings = byDepth.get(n.depth);
                    const idx = siblings.indexOf(n);
                    const theta = (idx / siblings.length) * 2 * Math.PI;
                    const r = n.depth === 0 ? 0 : (0.03 + (n.depth / maxDepth) * (RADIUS - 0.03));
                    result.push({
                        pos: new THREE.Vector3(Math.cos(theta) * r, Math.sin(theta) * r, n.depth * 0.008),
                        scale: n.children ? 0.007 : 0.005,
                    });
                }
                break;
            }
            case 'treemap': {
                d3treemap().size([SIZE, SIZE]).paddingInner(0.004)(h);
                for (const n of desc) {
                    const any = n;
                    if (any.x0 !== undefined) {
                        const cx = (any.x0 + any.x1) / 2 - SIZE / 2;
                        const cy = SIZE / 2 - (any.y0 + any.y1) / 2;
                        result.push({
                            pos: new THREE.Vector3(cx, cy, n.depth * 0.005),
                            scale: n.children ? 0.004 : 0.005,
                        });
                    }
                    else {
                        result.push({ pos: new THREE.Vector3(0, 0, 0), scale: 0.004 });
                    }
                }
                break;
            }
            case 'pack': {
                d3pack().size([SIZE, SIZE]).padding(0.003)(h);
                for (const n of desc) {
                    const any = n;
                    result.push({
                        pos: new THREE.Vector3(any.x - SIZE / 2, SIZE / 2 - any.y, 0),
                        scale: Math.max(0.003, any.r * 0.5),
                    });
                }
                break;
            }
        }
        return result;
    }
    function rebuildEdges(positions) {
        g.remove(edgeLines);
        edgeGeo.dispose();
        const h = hierarchy(rootData);
        const desc = h.descendants();
        const pts = [];
        h.links().forEach(link => {
            const si = desc.indexOf(link.source);
            const ti = desc.indexOf(link.target);
            if (si >= 0 && ti >= 0 && positions[si] && positions[ti]) {
                pts.push(positions[si].pos);
                pts.push(positions[ti].pos);
            }
        });
        edgeGeo = new THREE.BufferGeometry().setFromPoints(pts);
        edgeLines = new THREE.LineSegments(edgeGeo, edgeMat);
        edgeLines.userData.noHover = true;
        g.add(edgeLines);
    }
    // Initialize with tree layout
    currentPositions = computeLayout('tree');
    const mat4 = new THREE.Matrix4();
    currentPositions.forEach((p, i) => {
        mat4.compose(p.pos, new THREE.Quaternion(), new THREE.Vector3(p.scale, p.scale, p.scale));
        mesh.setMatrixAt(i, mat4);
    });
    mesh.instanceMatrix.needsUpdate = true;
    rebuildEdges(currentPositions);
    function morphTo(type) {
        const newPositions = computeLayout(type);
        // Hide edges during transition
        edgeLines.visible = false;
        activeTween = tweenInstanced(mesh, currentPositions, newPositions, 800, easeExpOut);
        label.text = LAYOUT_LABELS[type];
        label.sync();
        // Store a reference so we can rebuild edges when done
        const targetPositions = newPositions;
        const origTween = activeTween;
        const originalTick = origTween.tick.bind(origTween);
        activeTween = {
            tick: () => {
                originalTick();
                if (origTween.done) {
                    currentPositions = targetPositions;
                    rebuildEdges(currentPositions);
                    edgeLines.visible = true;
                }
            },
            get done() { return origTween.done; },
        };
    }
    return {
        group: g,
        tick: () => {
            if (activeTween && !activeTween.done) {
                activeTween.tick();
                if (activeTween.done)
                    activeTween = null;
            }
        },
        nextLayout: () => {
            currentIdx = (currentIdx + 1) % LAYOUT_CYCLE.length;
            morphTo(LAYOUT_CYCLE[currentIdx]);
        },
        currentType: () => LAYOUT_CYCLE[currentIdx],
        dispose: () => {
            mesh.dispose();
            nodeGeo.dispose();
            nodeMat.dispose();
            edgeGeo.dispose();
            edgeMat.dispose();
            label.dispose();
        },
    };
}
