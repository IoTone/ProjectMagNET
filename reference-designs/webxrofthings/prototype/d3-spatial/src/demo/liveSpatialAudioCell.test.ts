import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import * as THREE from 'three';
import { buildLiveSpatialAudioCell } from './liveSpatialAudioCell';

/* The audio path requires `window.__demo.audioListener` to find a real
 * THREE.AudioListener. In node tests we either skip audio init entirely
 * (no `window`) or provide a stub-listener that owns a fake AudioContext.
 *
 * The fake AudioContext is enough for `makeMusicLoopBuffer` to round-trip:
 * three.js's PositionalAudio + AudioListener accept anything that quacks
 * like BaseAudioContext, but tries to call `createGain` / `createBufferSource`
 * on it. To keep the test scope tight we use the no-listener path here
 * and assert the mesh-construction half of the cell. The audio path is
 * exercised by the dev/smoke runs in a real browser. */

beforeEach(() => {
  // Ensure no leftover __demo.audioListener from a previous test/module.
  if (typeof window !== 'undefined') {
    (window as any).__demo = undefined;
  }
});

afterEach(() => {
  if (typeof window !== 'undefined') {
    (window as any).__demo = undefined;
  }
});

describe('buildLiveSpatialAudioCell — mesh structure (no audio listener path)', () => {
  it('builds a Group containing body + two speakers + handle + LED', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(cell.group).toBeInstanceOf(THREE.Group);
    expect(cell.group.name).toBe('live-spatial-audio');
    // 5 meshes total: body, speaker L, speaker R, handle, LED.
    const meshes = cell.group.children.filter(c => (c as THREE.Mesh).isMesh);
    expect(meshes.length).toBe(5);
    cell.dispose();
  });

  it('isPlaying() reports false when no AudioListener was wired', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(cell.isPlaying()).toBe(false);
    cell.dispose();
  });

  it('play() resolves silently when no audio listener is available', async () => {
    const cell = buildLiveSpatialAudioCell();
    // No __demo.audioListener → no PositionalAudio created → play() is a
    // safe no-op rather than throwing.
    await expect(cell.play()).resolves.toBeUndefined();
    expect(cell.isPlaying()).toBe(false);
    cell.dispose();
  });

  it('pause() is a no-op when nothing is playing', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(() => cell.pause()).not.toThrow();
    cell.dispose();
  });

  it('setGain clamps to [0, 1] silently when no audio is wired', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(() => cell.setGain(-5)).not.toThrow();
    expect(() => cell.setGain(99)).not.toThrow();
    cell.dispose();
  });

  it('tick() is a no-op — audio spatialises from group transform automatically', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(() => cell.tick(0)).not.toThrow();
    expect(() => cell.tick(1.5)).not.toThrow();
    cell.dispose();
  });

  it('dispose() is idempotent', () => {
    const cell = buildLiveSpatialAudioCell();
    cell.dispose();
    expect(() => cell.dispose()).not.toThrow();
  });

  it('respects autoplay=false — no play attempt on construction', () => {
    const cell = buildLiveSpatialAudioCell({ autoplay: false });
    expect(cell.isPlaying()).toBe(false);
    cell.dispose();
  });

  it('size opt scales the body geometry', () => {
    const smallCell = buildLiveSpatialAudioCell({ size: 0.04 });
    const bigCell   = buildLiveSpatialAudioCell({ size: 0.16 });
    const smallBody = smallCell.group.children.find(c => (c as THREE.Mesh).isMesh) as THREE.Mesh;
    const bigBody   = bigCell.group.children.find(c => (c as THREE.Mesh).isMesh) as THREE.Mesh;
    const smallGeo = smallBody.geometry as THREE.BoxGeometry;
    const bigGeo   = bigBody.geometry   as THREE.BoxGeometry;
    expect(bigGeo.parameters.width).toBeGreaterThan(smallGeo.parameters.width);
    smallCell.dispose();
    bigCell.dispose();
  });
});

describe('proceduralMusic buffer (exercised indirectly)', () => {
  // makeMusicLoopBuffer needs a BaseAudioContext-conformant input. Node
  // doesn't ship one; we'd need to mock createBuffer + getChannelData. The
  // function is small + pure, with no branching on context behaviour — a
  // smoke-shot in the browser covers it. Keep this placeholder as a
  // reminder to add a coverage path if the maths ever changes shape.
  it.skip('makeMusicLoopBuffer round-trip (browser-only)', () => {});
});
