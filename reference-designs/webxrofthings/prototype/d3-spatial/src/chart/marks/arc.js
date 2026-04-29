import * as THREE from 'three';
export function buildArcMark(opts = {}) {
    const { color = 0xcc99ff, radius = 0.12, tubeRadius = 0.004, startAngle = -Math.PI * 0.75, endAngle = Math.PI * 0.25, segments = 96, } = opts;
    const g = new THREE.Group();
    g.name = 'arc-mark';
    const pts = [];
    for (let i = 0; i <= segments; i++) {
        const t = i / segments;
        const a = startAngle + (endAngle - startAngle) * t;
        pts.push(new THREE.Vector3(Math.cos(a) * radius, Math.sin(a) * radius, 0));
    }
    const curve = new THREE.CatmullRomCurve3(pts);
    const geo = new THREE.TubeGeometry(curve, segments, tubeRadius, 8, false);
    const mat = new THREE.MeshStandardMaterial({
        color,
        roughness: 0.4,
        metalness: 0.1,
        emissive: color,
        emissiveIntensity: 0.35,
    });
    const mesh = new THREE.Mesh(geo, mat);
    g.add(mesh);
    const tickMat = new THREE.MeshBasicMaterial({ color, transparent: true, opacity: 0.6 });
    const tickGeo = new THREE.SphereGeometry(tubeRadius * 1.8, 8, 8);
    const ticks = 6;
    for (let i = 0; i <= ticks; i++) {
        const a = startAngle + (endAngle - startAngle) * (i / ticks);
        const tick = new THREE.Mesh(tickGeo, tickMat);
        tick.position.set(Math.cos(a) * radius, Math.sin(a) * radius, 0);
        g.add(tick);
    }
    return g;
}
