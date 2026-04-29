import * as THREE from 'three';
export function buildLineMark(points, opts = {}) {
    const { color = 0x66ffcc, radius = 0.004, segments = 8, tubularSegments, curveTension = 0.5, } = opts;
    const g = new THREE.Group();
    g.name = 'line-mark';
    if (points.length < 2)
        return g;
    const curve = new THREE.CatmullRomCurve3(points, false, 'catmullrom', curveTension);
    const tubSeg = tubularSegments ?? Math.max(64, points.length * 2);
    const geo = new THREE.TubeGeometry(curve, tubSeg, radius, segments, false);
    const mat = new THREE.MeshStandardMaterial({
        color,
        roughness: 0.5,
        metalness: 0.1,
        emissive: color,
        emissiveIntensity: 0.25,
    });
    const mesh = new THREE.Mesh(geo, mat);
    g.add(mesh);
    return g;
}
/**
 * Rebuild the TubeGeometry inside an existing line-mark group with new points.
 * Disposes old geometry and creates a new one for smooth data updates.
 */
export function updateLineMark(group, points, opts = {}) {
    const { radius = 0.004, segments = 8, tubularSegments, curveTension = 0.5, } = opts;
    if (points.length < 2)
        return;
    // Find the existing mesh
    const oldMesh = group.children[0];
    if (!oldMesh)
        return;
    // Dispose old geometry
    oldMesh.geometry.dispose();
    // Build new geometry
    const curve = new THREE.CatmullRomCurve3(points, false, 'catmullrom', curveTension);
    const tubSeg = tubularSegments ?? Math.max(64, points.length * 2);
    const newGeo = new THREE.TubeGeometry(curve, tubSeg, radius, segments, false);
    oldMesh.geometry = newGeo;
}
