import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import * as THREE from 'three';
import { easeExpOut, tweenInstanced, tweenMeshes, tweenInstancedBoxes } from './tween';

describe('easeExpOut', () => {
  it('clamps t=0 to 0', () => {
    expect(easeExpOut(0)).toBe(0);
  });

  it('clamps t=1 to exactly 1 (no float drift)', () => {
    expect(easeExpOut(1)).toBe(1);
  });

  it('is monotonically increasing on [0, 1]', () => {
    let prev = easeExpOut(0);
    for (let t = 0.1; t <= 1; t += 0.1) {
      const v = easeExpOut(t);
      expect(v).toBeGreaterThanOrEqual(prev);
      prev = v;
    }
  });

  it('is fast at the start (>70% of the way at t=0.2)', () => {
    expect(easeExpOut(0.2)).toBeGreaterThan(0.7);
  });
});

describe('tweenInstanced', () => {
  let mesh: THREE.InstancedMesh;
  let nowSpy: ReturnType<typeof vi.spyOn>;
  let nowMs = 0;

  beforeEach(() => {
    nowMs = 1000;
    nowSpy = vi.spyOn(performance, 'now').mockImplementation(() => nowMs);
    mesh = new THREE.InstancedMesh(new THREE.SphereGeometry(1), new THREE.MeshBasicMaterial(), 3);
  });

  afterEach(() => {
    nowSpy.mockRestore();
    mesh.dispose();
  });

  it('completes after the duration elapses', () => {
    const from = [
      { pos: new THREE.Vector3(0, 0, 0), scale: 1 },
      { pos: new THREE.Vector3(1, 0, 0), scale: 1 },
      { pos: new THREE.Vector3(0, 1, 0), scale: 1 },
    ];
    const to = [
      { pos: new THREE.Vector3(2, 0, 0), scale: 2 },
      { pos: new THREE.Vector3(3, 0, 0), scale: 2 },
      { pos: new THREE.Vector3(0, 3, 0), scale: 2 },
    ];
    const t = tweenInstanced(mesh, from, to, 500);

    nowMs = 1100;
    t.tick();
    expect(t.done).toBe(false);

    nowMs = 1500; // exactly duration
    t.tick();
    expect(t.done).toBe(true);

    nowMs = 9999; // way past — still done
    t.tick();
    expect(t.done).toBe(true);
  });

  it('writes interpolated matrices to instanceMatrix', () => {
    const from = [{ pos: new THREE.Vector3(0, 0, 0), scale: 1 }];
    const to = [{ pos: new THREE.Vector3(10, 0, 0), scale: 1 }];
    const tw = tweenInstanced(mesh, from, to, 100);

    nowMs = 1100; // t=1, fully there
    tw.tick();

    const m = new THREE.Matrix4();
    mesh.getMatrixAt(0, m);
    const pos = new THREE.Vector3().setFromMatrixPosition(m);
    expect(pos.x).toBeCloseTo(10, 5);
    // BufferAttribute.needsUpdate is set-only — verify via version bump
    expect(mesh.instanceMatrix.version).toBeGreaterThan(0);
  });

  it('respects min(from.length, to.length, mesh.count)', () => {
    const from = [
      { pos: new THREE.Vector3(0, 0, 0), scale: 1 },
      { pos: new THREE.Vector3(0, 0, 0), scale: 1 },
    ];
    const to = [
      { pos: new THREE.Vector3(5, 0, 0), scale: 1 },
    ];
    // mesh.count is 3, but to[] only has 1 entry — should not blow up
    const tw = tweenInstanced(mesh, from, to, 100);
    nowMs = 1100;
    expect(() => tw.tick()).not.toThrow();
  });
});

describe('tweenMeshes', () => {
  it('lerps position and scale per mesh', () => {
    const meshes = [new THREE.Mesh(new THREE.BoxGeometry(), new THREE.MeshBasicMaterial())];
    const from = [{ pos: new THREE.Vector3(0, 0, 0), scale: new THREE.Vector3(1, 1, 1) }];
    const to = [{ pos: new THREE.Vector3(4, 0, 0), scale: new THREE.Vector3(2, 2, 2) }];

    let nowMs = 0;
    const spy = vi.spyOn(performance, 'now').mockImplementation(() => nowMs);
    const tw = tweenMeshes(meshes, from, to, 200);

    nowMs = 200;
    tw.tick();
    expect(meshes[0]!.position.x).toBeCloseTo(4, 5);
    expect(meshes[0]!.scale.x).toBeCloseTo(2, 5);
    expect(tw.done).toBe(true);

    spy.mockRestore();
  });
});

describe('tweenInstancedBoxes', () => {
  it('handles independent 3-axis scale per instance', () => {
    const mesh = new THREE.InstancedMesh(new THREE.BoxGeometry(), new THREE.MeshBasicMaterial(), 1);
    const from = [{ pos: new THREE.Vector3(0, 0, 0), scale: new THREE.Vector3(1, 1, 1) }];
    const to = [{ pos: new THREE.Vector3(0, 0, 0), scale: new THREE.Vector3(2, 4, 8) }];

    let nowMs = 0;
    const spy = vi.spyOn(performance, 'now').mockImplementation(() => nowMs);
    const tw = tweenInstancedBoxes(mesh, from, to, 100);

    nowMs = 100;
    tw.tick();

    const m = new THREE.Matrix4();
    mesh.getMatrixAt(0, m);
    const scl = new THREE.Vector3();
    m.decompose(new THREE.Vector3(), new THREE.Quaternion(), scl);
    expect(scl.x).toBeCloseTo(2, 4);
    expect(scl.y).toBeCloseTo(4, 4);
    expect(scl.z).toBeCloseTo(8, 4);

    spy.mockRestore();
    mesh.dispose();
  });
});
