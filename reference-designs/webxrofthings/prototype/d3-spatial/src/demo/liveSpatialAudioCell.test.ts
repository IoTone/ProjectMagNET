import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import * as THREE from 'three';

/* The audio path requires `window.__demo.audioListener` to find a real
 * THREE.AudioListener. In node tests we either skip audio init entirely
 * (no `window`) or provide a stub-listener that owns a fake AudioContext.
 *
 * For these tests we use the no-listener path so the audio init silently
 * no-ops and we can assert the visual mesh structure + lifecycle. The
 * audio path itself is exercised by the dev/smoke runs in a real browser.
 *
 * troika-three-text needs a `self` global the node env doesn't have;
 * stub it for the boombox theme label. Same dodge the splat-gallery +
 * IMU tests use. */
vi.mock('troika-three-text', () => {
  class StubText extends THREE.Object3D {
    text = '';
    fontSize = 0.018;
    color: number = 0xffffff;
    anchorX: string = 'center';
    anchorY: string = 'middle';
    sync() {}
    dispose() {}
  }
  return { Text: StubText };
});

import { buildLiveSpatialAudioCell } from './liveSpatialAudioCell';

beforeEach(() => {
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
  it('builds a Group named live-spatial-audio with wireframe boombox + icons', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(cell.group).toBeInstanceOf(THREE.Group);
    expect(cell.group.name).toBe('live-spatial-audio');
    // Top-level children include: body (LineSegments), L speaker (LineLoop),
    // R speaker (LineLoop), handle (LineSegments), play icon (Mesh),
    // pause group (Group of 2 Mesh), theme label (Text stub), theme sublabel
    // (Text stub), particle group (Group). Asserting `> 5` keeps the test
    // robust against minor mesh-count shuffles while still catching a
    // catastrophic regression where construction returns an empty group.
    expect(cell.group.children.length).toBeGreaterThan(5);
    cell.dispose();
  });

  it('isPlaying() reports false on construction (autoplay default off)', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(cell.isPlaying()).toBe(false);
    cell.dispose();
  });

  it('play() resolves silently when no audio listener is available', async () => {
    const cell = buildLiveSpatialAudioCell();
    await expect(cell.play()).resolves.toBeUndefined();
    expect(cell.isPlaying()).toBe(false);
    cell.dispose();
  });

  it('pause() is a no-op when nothing is playing', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(() => cell.pause()).not.toThrow();
    cell.dispose();
  });

  it('setGain is silent when no audio is wired', () => {
    const cell = buildLiveSpatialAudioCell();
    expect(() => cell.setGain(-5)).not.toThrow();
    expect(() => cell.setGain(99)).not.toThrow();
    cell.dispose();
  });

  it('tick() is a no-op — particles animate via onBeforeRender', () => {
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
    // Body is the first LineSegments child (wireframe box).
    const small = smallCell.group.children.find(c => c instanceof THREE.LineSegments) as THREE.LineSegments;
    const big   = bigCell.group.children.find(c => c instanceof THREE.LineSegments) as THREE.LineSegments;
    // EdgesGeometry derives from a BoxGeometry that scales with `size`;
    // compare bounding box widths so we don't depend on parameters object.
    small.geometry.computeBoundingBox();
    big.geometry.computeBoundingBox();
    expect(big.geometry.boundingBox!.max.x - big.geometry.boundingBox!.min.x)
      .toBeGreaterThan(small.geometry.boundingBox!.max.x - small.geometry.boundingBox!.min.x);
    smallCell.dispose();
    bigCell.dispose();
  });
});

describe('proceduralMusic — theme registry', () => {
  it('pickRandomTheme returns one of the four supported themes', async () => {
    const { pickRandomTheme, MUSIC_THEME_LABELS } = await import('../audio/proceduralMusic');
    const seen = new Set<string>();
    for (let i = 0; i < 50; i++) seen.add(pickRandomTheme());
    // 50 draws from a 4-element set should reliably hit at least 2 distinct values.
    expect(seen.size).toBeGreaterThanOrEqual(2);
    for (const theme of seen) {
      expect(MUSIC_THEME_LABELS[theme as keyof typeof MUSIC_THEME_LABELS]).toBeDefined();
    }
  });
});
