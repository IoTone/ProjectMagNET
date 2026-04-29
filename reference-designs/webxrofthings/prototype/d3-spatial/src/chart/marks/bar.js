import * as THREE from 'three';
export function buildBarMark(points, baseline, opts = {}) {
    const { color = 0x66ccff, width = 0.008, depth = 0.008 } = opts;
    const g = new THREE.Group();
    g.name = 'bar-mark';
    if (points.length === 0)
        return g;
    const geo = new THREE.BoxGeometry(1, 1, 1);
    const mat = new THREE.MeshStandardMaterial({
        color,
        roughness: 0.45,
        metalness: 0.1,
        emissive: color,
        emissiveIntensity: 0.2,
    });
    const mesh = new THREE.InstancedMesh(geo, mat, points.length);
    mesh.name = 'bar-instances';
    const m = new THREE.Matrix4();
    for (let i = 0; i < points.length; i++) {
        const p = points[i];
        const h = Math.max(1e-4, p.y - baseline);
        m.compose(new THREE.Vector3(p.x, baseline + h / 2, 0), new THREE.Quaternion(), new THREE.Vector3(width, h, depth));
        mesh.setMatrixAt(i, m);
    }
    mesh.instanceMatrix.needsUpdate = true;
    g.add(mesh);
    return g;
}
/**
 * Update an existing bar-mark group's InstancedMesh with new data points.
 * Rebuilds the InstancedMesh if the count changed, otherwise just updates matrices.
 */
export function updateBarMark(group, points, baseline, opts = {}) {
    const { color = 0x66ccff, width = 0.008, depth = 0.008 } = opts;
    if (points.length === 0)
        return;
    const oldMesh = group.children[0];
    if (!oldMesh || oldMesh.count !== points.length) {
        // Count changed — full rebuild
        group.clear();
        if (oldMesh) {
            oldMesh.geometry.dispose();
            oldMesh.material.dispose();
        }
        group.add(buildBarMark(points, baseline, opts).children[0]);
        return;
    }
    // Same count — just update matrices (tween-like instant update)
    const m = new THREE.Matrix4();
    for (let i = 0; i < points.length; i++) {
        const p = points[i];
        const h = Math.max(1e-4, p.y - baseline);
        m.compose(new THREE.Vector3(p.x, baseline + h / 2, 0), new THREE.Quaternion(), new THREE.Vector3(width, h, depth));
        oldMesh.setMatrixAt(i, m);
    }
    oldMesh.instanceMatrix.needsUpdate = true;
}
