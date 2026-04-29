import * as THREE from 'three';
/** Exponential-out easing: fast start, gentle settle. */
export function easeExpOut(t) {
    return t >= 1 ? 1 : 1 - Math.pow(2, -10 * t);
}
/**
 * Tween InstancedMesh instance positions (uniform scale) from one set of
 * positions/scales to another over `duration` ms.
 *
 * Call `tick()` every frame.  Returns an object with { tick, done }.
 */
export function tweenInstanced(mesh, from, to, duration, easing = easeExpOut) {
    const startTime = performance.now();
    const count = Math.min(from.length, to.length, mesh.count);
    const m = new THREE.Matrix4();
    const q = new THREE.Quaternion();
    let _done = false;
    function tick() {
        const elapsed = performance.now() - startTime;
        let t = Math.min(elapsed / duration, 1);
        t = easing(t);
        for (let i = 0; i < count; i++) {
            const fp = from[i];
            const tp = to[i];
            const x = fp.pos.x + (tp.pos.x - fp.pos.x) * t;
            const y = fp.pos.y + (tp.pos.y - fp.pos.y) * t;
            const z = fp.pos.z + (tp.pos.z - fp.pos.z) * t;
            const s = fp.scale + (tp.scale - fp.scale) * t;
            m.compose(new THREE.Vector3(x, y, z), q, new THREE.Vector3(s, s, s));
            mesh.setMatrixAt(i, m);
        }
        mesh.instanceMatrix.needsUpdate = true;
        if (t >= 1)
            _done = true;
    }
    return { tick, get done() { return _done; } };
}
/**
 * Tween an array of individual THREE.Mesh objects from one position+scale
 * to another over `duration` ms.
 */
export function tweenMeshes(meshes, from, to, duration, easing = easeExpOut) {
    const startTime = performance.now();
    const count = Math.min(from.length, to.length, meshes.length);
    let _done = false;
    function tick() {
        const elapsed = performance.now() - startTime;
        let t = Math.min(elapsed / duration, 1);
        t = easing(t);
        for (let i = 0; i < count; i++) {
            const fp = from[i];
            const tp = to[i];
            const mesh = meshes[i];
            mesh.position.lerpVectors(fp.pos, tp.pos, t);
            mesh.scale.lerpVectors(fp.scale, tp.scale, t);
        }
        if (t >= 1)
            _done = true;
    }
    return { tick, get done() { return _done; } };
}
/**
 * Tween InstancedMesh where each instance can have independent 3-axis scale
 * (for treemap box cells).
 */
export function tweenInstancedBoxes(mesh, from, to, duration, easing = easeExpOut) {
    const startTime = performance.now();
    const count = Math.min(from.length, to.length, mesh.count);
    const m = new THREE.Matrix4();
    const q = new THREE.Quaternion();
    const tmpPos = new THREE.Vector3();
    const tmpScl = new THREE.Vector3();
    let _done = false;
    function tick() {
        const elapsed = performance.now() - startTime;
        let t = Math.min(elapsed / duration, 1);
        t = easing(t);
        for (let i = 0; i < count; i++) {
            const fp = from[i];
            const tp = to[i];
            tmpPos.lerpVectors(fp.pos, tp.pos, t);
            tmpScl.lerpVectors(fp.scale, tp.scale, t);
            m.compose(tmpPos, q, tmpScl);
            mesh.setMatrixAt(i, m);
        }
        mesh.instanceMatrix.needsUpdate = true;
        if (t >= 1)
            _done = true;
    }
    return { tick, get done() { return _done; } };
}
