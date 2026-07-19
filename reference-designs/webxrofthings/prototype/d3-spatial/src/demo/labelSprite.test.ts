import { describe, it, expect } from 'vitest';
import * as THREE from 'three';
import { makeLabelSprite, assignLabelTiers } from './labelSprite';

describe('labelSprite', () => {
  it('is DOM-gated: returns null in headless environments', () => {
    expect(typeof document).toBe('undefined');
    expect(makeLabelSprite({ text: '東京' })).toBeNull();
  });

  /* Regression: the XR beam raycast (xrRig.update) sweeps the whole scene
   * with a hand-built ray — no raycaster.camera. THREE.Sprite.raycast()
   * THROWS in that case, which killed the render loop in-headset the
   * moment the first label sprite appeared (frame froze → the stuck image
   * followed the head, reading as "the view is locked to the camera").
   * Label sprites must therefore be raycast-inert. */
  it('a raw THREE.Sprite throws under a camera-less raycast (the crash we guard against)', () => {
    const scene = new THREE.Scene();
    scene.add(new THREE.Sprite(new THREE.SpriteMaterial()));
    const ray = new THREE.Raycaster();
    ray.ray.origin.set(0, 0, 5);
    ray.ray.direction.set(0, 0, -1);
    // three r161 logs the camera warning then hard-throws a TypeError on
    // the null camera's matrixWorld — this is the exact in-headset crash.
    expect(() => ray.intersectObjects(scene.children, true)).toThrow(/matrixWorld|camera/i);
  });

  it('a raycast-neutered sprite is safe under the same sweep', () => {
    const scene = new THREE.Scene();
    const sprite = new THREE.Sprite(new THREE.SpriteMaterial());
    sprite.raycast = () => {}; // what makeLabelSprite does
    scene.add(sprite);
    const ray = new THREE.Raycaster();
    ray.ray.origin.set(0, 0, 5);
    ray.ray.direction.set(0, 0, -1);
    expect(() => ray.intersectObjects(scene.children, true)).not.toThrow();
  });
});

describe('assignLabelTiers', () => {
  const W = 0.12, H = 0.03;

  it('isolated labels all sit on tier 0', () => {
    const tiers = assignLabelTiers([
      { id: 'a', x: 0, y: 0 },
      { id: 'b', x: 1, y: 0 },
      { id: 'c', x: 0, y: 1 },
    ], W, H);
    expect([...tiers.values()]).toEqual([0, 0, 0]);
  });

  it('crowded labels fan out to distinct tiers', () => {
    const tiers = assignLabelTiers([
      { id: 'b', x: 0.001, y: 0.001 },
      { id: 'a', x: 0.002, y: 0.002 },
      { id: 'c', x: 0.003, y: 0.000 },
    ], W, H);
    const values = [tiers.get('a'), tiers.get('b'), tiers.get('c')].sort();
    expect(values).toEqual([0, 1, 2]);
  });

  it('tiers are stable across polls when the neighborhood is unchanged (id-sorted)', () => {
    const items = [
      { id: 'veh-2', x: 0.01, y: 0.0 },
      { id: 'veh-1', x: 0.02, y: 0.01 },
    ];
    const t1 = assignLabelTiers(items, W, H);
    const t2 = assignLabelTiers([...items].reverse(), W, H);
    expect(t1.get('veh-1')).toBe(t2.get('veh-1'));
    expect(t1.get('veh-2')).toBe(t2.get('veh-2'));
    expect(t1.get('veh-1')).not.toBe(t1.get('veh-2'));
  });
});
