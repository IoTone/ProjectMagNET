/**
 * Tests target the navigation state machine + lifecycle of the gallery cell.
 *
 * What's NOT tested here:
 *   - The actual SplatMesh load (needs a real WebGL context + fetchable .sog).
 *     We mock the SplatMesh constructor to a stub that resolves `initialized`
 *     to a never-failing Promise, which is enough to exercise next/prev/goTo.
 *   - The auto-rotation onBeforeRender hook (requires a renderer to drive it).
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import * as THREE from 'three';

// Mock @mkkellogg/gaussian-splats-3d BEFORE the cell imports it.
// DropInViewer is a THREE.Group subclass in real life; for tests a stub
// with the addSplatScene / removeSplatScene API shape is enough.
vi.mock('@mkkellogg/gaussian-splats-3d', () => {
  class StubDropInViewer extends THREE.Group {
    sceneCount = 0;
    loadedUrls: string[] = [];
    disposed = false;
    constructor(_opts: Record<string, unknown> = {}) { super(); }
    addSplatScene(path: string, _opts: Record<string, unknown> = {}): Promise<void> {
      this.loadedUrls.push(path);
      this.sceneCount++;
      return Promise.resolve();
    }
    removeSplatScene(_index: number, _showUI?: boolean): Promise<void> {
      this.sceneCount = Math.max(0, this.sceneCount - 1);
      return Promise.resolve();
    }
    dispose() { this.disposed = true; }
  }
  return { DropInViewer: StubDropInViewer };
});

// Mock troika-three-text — it expects browser `self`/`Worker` globals that
// node's vitest environment doesn't have. The cell only consumes a tiny
// surface (text/color/position/sync/dispose), so a stub keeps the test
// hermetic without dragging in jsdom.
vi.mock('troika-three-text', () => {
  class StubText extends THREE.Object3D {
    text = '';
    fontSize = 0.018;
    color: number = 0xffffff;
    anchorX = 'center';
    anchorY = 'top';
    sync() { /* no-op in tests */ }
    dispose() { /* no-op in tests */ }
  }
  return { Text: StubText };
});

import { buildLiveSplatGalleryCell, type SplatPhoto } from './liveSplatGalleryCell';

const PHOTOS: SplatPhoto[] = [
  { url: '/spatial/a.sog', title: 'A' },
  { url: '/spatial/b.sog', title: 'B' },
  { url: '/spatial/c.sog', title: 'C' },
];

beforeEach(() => { vi.useFakeTimers(); });
afterEach(() => { vi.useRealTimers(); vi.clearAllMocks(); });

describe('buildLiveSplatGalleryCell — construction', () => {
  it('throws when photos is empty', () => {
    expect(() => buildLiveSplatGalleryCell({ photos: [] })).toThrow();
  });

  it('builds with currentIndex 0 and the first photo loaded', () => {
    const cell = buildLiveSplatGalleryCell({ photos: PHOTOS, autoAdvanceMs: 0, bindKeyboard: false });
    expect(cell.currentIndex()).toBe(0);
    expect(cell.group).toBeInstanceOf(THREE.Group);
    cell.dispose();
  });
});

describe('navigation', () => {
  it('next() advances by one and wraps past the end', () => {
    const cell = buildLiveSplatGalleryCell({ photos: PHOTOS, autoAdvanceMs: 0, bindKeyboard: false });
    expect(cell.currentIndex()).toBe(0);
    cell.next(); expect(cell.currentIndex()).toBe(1);
    cell.next(); expect(cell.currentIndex()).toBe(2);
    cell.next(); expect(cell.currentIndex()).toBe(0);  // wrap
    cell.dispose();
  });

  it('prev() decrements by one and wraps past the start', () => {
    const cell = buildLiveSplatGalleryCell({ photos: PHOTOS, autoAdvanceMs: 0, bindKeyboard: false });
    cell.prev(); expect(cell.currentIndex()).toBe(2);  // wrap from 0 → 2
    cell.prev(); expect(cell.currentIndex()).toBe(1);
    cell.dispose();
  });

  it('goTo(index) jumps directly, wrapping out-of-range values', () => {
    const cell = buildLiveSplatGalleryCell({ photos: PHOTOS, autoAdvanceMs: 0, bindKeyboard: false });
    cell.goTo(2); expect(cell.currentIndex()).toBe(2);
    cell.goTo(5); expect(cell.currentIndex()).toBe(2);   // 5 % 3 = 2
    cell.goTo(-1); expect(cell.currentIndex()).toBe(2);  // -1 wraps to last
    cell.goTo(0); expect(cell.currentIndex()).toBe(0);
    cell.dispose();
  });

  it('goTo(currentIndex) is a no-op (no reload churn)', () => {
    // Check that requesting the already-current index doesn't change state.
    // The stub's load is synchronous so we can compare child counts before/after.
    const cell = buildLiveSplatGalleryCell({ photos: PHOTOS, autoAdvanceMs: 0, bindKeyboard: false });
    const before = cell.group.children.length;
    cell.goTo(0);
    expect(cell.currentIndex()).toBe(0);
    expect(cell.group.children.length).toBe(before);
    cell.dispose();
  });
});

describe('auto-advance timer', () => {
  it('advances on each autoAdvanceMs tick', async () => {
    const cell = buildLiveSplatGalleryCell({
      photos: PHOTOS, autoAdvanceMs: 1000, bindKeyboard: false,
    });
    // Cells are now lazy — they don't kick off the auto-advance timer
    // until something flips them on (applyShowOnly / initial-activation
    // on manifest load). The test simulates that by calling setActive.
    cell.setActive(true);
    expect(cell.currentIndex()).toBe(0);
    await vi.advanceTimersByTimeAsync(1000); expect(cell.currentIndex()).toBe(1);
    await vi.advanceTimersByTimeAsync(1000); expect(cell.currentIndex()).toBe(2);
    await vi.advanceTimersByTimeAsync(1000); expect(cell.currentIndex()).toBe(0);
    cell.dispose();
  });

  it('autoAdvanceMs = 0 disables the timer', async () => {
    const cell = buildLiveSplatGalleryCell({
      photos: PHOTOS, autoAdvanceMs: 0, bindKeyboard: false,
    });
    await vi.advanceTimersByTimeAsync(60_000);
    expect(cell.currentIndex()).toBe(0);
    cell.dispose();
  });

  it('manual navigation resets the auto-advance interval', async () => {
    const cell = buildLiveSplatGalleryCell({
      photos: PHOTOS, autoAdvanceMs: 1000, bindKeyboard: false,
    });
    cell.setActive(true);
    // Half-way through the interval, manually advance — the timer should
    // restart from zero rather than fire 500ms later.
    await vi.advanceTimersByTimeAsync(500);
    cell.next();
    expect(cell.currentIndex()).toBe(1);
    await vi.advanceTimersByTimeAsync(500);
    expect(cell.currentIndex()).toBe(1);   // hasn't ticked yet
    await vi.advanceTimersByTimeAsync(500);
    expect(cell.currentIndex()).toBe(2);   // now it has
    cell.dispose();
  });

  it('inactive cell does not auto-advance even with autoAdvanceMs > 0', async () => {
    // The new lazy lifecycle: a cell that was never setActive(true) must
    // not touch its auto-advance timer. This is the load-bearing guarantee
    // for "no SOG cycling while user is on a different mode" — without it,
    // the Flight Info / Music / Video views were paying the GPU cost of
    // a photo swap every 15 s in the background.
    const cell = buildLiveSplatGalleryCell({
      photos: PHOTOS, autoAdvanceMs: 1000, bindKeyboard: false,
    });
    await vi.advanceTimersByTimeAsync(60_000);
    expect(cell.currentIndex()).toBe(0);
    cell.dispose();
  });

  it('setActive(false) halts auto-advance without disposing', async () => {
    const cell = buildLiveSplatGalleryCell({
      photos: PHOTOS, autoAdvanceMs: 1000, bindKeyboard: false,
    });
    cell.setActive(true);
    await vi.advanceTimersByTimeAsync(1000); expect(cell.currentIndex()).toBe(1);
    cell.setActive(false);
    await vi.advanceTimersByTimeAsync(60_000);
    expect(cell.currentIndex()).toBe(1);   // frozen at the moment of deactivation
    cell.dispose();
  });
});

// Keyboard wiring tests intentionally omitted — the cell guards
// `typeof window !== 'undefined'` and vitest's node env has no window,
// so we'd be testing the "browser-only branch ignored in node" path.
// Manual verification: in the d3-spatial dev server, hit ArrowLeft /
// ArrowRight while a splat gallery mark is loaded.

describe('lifecycle', () => {
  it('dispose() stops the auto-advance timer', async () => {
    const cell = buildLiveSplatGalleryCell({
      photos: PHOTOS, autoAdvanceMs: 1000, bindKeyboard: false,
    });
    cell.dispose();
    await vi.advanceTimersByTimeAsync(5000);
    // No throws; nothing further to assert on a disposed cell.
    expect(cell.currentIndex()).toBe(0);
  });

  it('tick() is safe to call after dispose', () => {
    const cell = buildLiveSplatGalleryCell({
      photos: PHOTOS, autoAdvanceMs: 0, bindKeyboard: false,
    });
    cell.dispose();
    expect(() => cell.tick(1)).not.toThrow();
  });
});
