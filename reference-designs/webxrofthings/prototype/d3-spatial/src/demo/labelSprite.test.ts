import { describe, it, expect } from 'vitest';
import * as THREE from 'three';
import { makeLabelSprite } from './labelSprite';

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
