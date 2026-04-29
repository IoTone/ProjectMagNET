import * as THREE from 'three';
export function buildScatterMark(points, opts = {}) {
    const { color = 0xffcc66, radius = 0.006, radii } = opts;
    const g = new THREE.Group();
    g.name = 'scatter-mark';
    if (points.length === 0)
        return g;
    const geo = new THREE.SphereGeometry(1, 12, 12);
    const mat = new THREE.MeshStandardMaterial({
        color,
        roughness: 0.4,
        metalness: 0.1,
        emissive: color,
        emissiveIntensity: 0.25,
    });
    const mesh = new THREE.InstancedMesh(geo, mat, points.length);
    mesh.name = 'scatter-instances';
    const m = new THREE.Matrix4();
    for (let i = 0; i < points.length; i++) {
        const p = points[i];
        const r = radii?.[i] ?? radius;
        m.compose(p, new THREE.Quaternion(), new THREE.Vector3(r, r, r));
        mesh.setMatrixAt(i, m);
    }
    mesh.instanceMatrix.needsUpdate = true;
    g.add(mesh);
    return g;
}
