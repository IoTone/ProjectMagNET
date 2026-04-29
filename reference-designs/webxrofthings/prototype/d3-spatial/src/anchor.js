import * as THREE from 'three';
export function createAnchor() {
    const g = new THREE.Group();
    g.name = 'placeholder';
    const cube = new THREE.Mesh(new THREE.BoxGeometry(0.08, 0.08, 0.08), new THREE.MeshStandardMaterial({ color: 0x336699, roughness: 0.5, metalness: 0.1 }));
    cube.position.set(0, -0.1, 0);
    g.add(cube);
    const wire = new THREE.LineSegments(new THREE.EdgesGeometry(cube.geometry), new THREE.LineBasicMaterial({ color: 0x66ffff, transparent: true, opacity: 0.6 }));
    cube.add(wire);
    return g;
}
