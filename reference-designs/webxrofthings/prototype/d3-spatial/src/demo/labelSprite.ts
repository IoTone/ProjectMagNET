/**
 * Canvas-backed text sprites for in-scene hovering labels.
 *
 * Why canvas + THREE.Sprite (and not troika/three-mesh-ui):
 *  - The browser's 2D canvas rasterizes CJK with the system font stack —
 *    kanji labels need no bundled Japanese font (troika's default font has
 *    no CJK glyphs; a Noto Sans JP subset would be a ~MB asset).
 *  - Sprites billboard toward the camera for free — "hovering label" reads
 *    correctly from any seat position.
 *  - One small texture per label; redraw only when the text changes.
 *
 * DOM-gated: in headless/node test environments (no document) `makeLabelSprite`
 * returns null and callers simply skip labels — same pattern as the legend
 * canvases in the geo cells.
 */

import * as THREE from 'three';

export interface LabelSprite {
  sprite: THREE.Sprite;
  /** Redraws the canvas iff the text actually changed. */
  setText(text: string): void;
  dispose(): void;
}

export interface LabelSpriteOpts {
  text?: string;
  /** World height of the label. Width follows the canvas aspect. Default 0.022. */
  height?: number;
  /** CSS color for the text. Default warm cream (TEXT.body). */
  color?: string;
  /** CSS color for the halo/outline that keeps text readable on any backdrop. */
  haloColor?: string;
  fontPx?: number;
}

const CANVAS_W = 256;
const CANVAS_H = 64;

export function makeLabelSprite(opts: LabelSpriteOpts = {}): LabelSprite | null {
  if (typeof document === 'undefined') return null;
  const {
    text = '',
    height = 0.022,
    color = '#f5e9c8',
    haloColor = 'rgba(10,12,16,0.85)',
    fontPx = 34,
  } = opts;

  const canvas = document.createElement('canvas');
  canvas.width = CANVAS_W;
  canvas.height = CANVAS_H;
  const tex = new THREE.CanvasTexture(canvas);
  tex.colorSpace = THREE.SRGBColorSpace;
  const mat = new THREE.SpriteMaterial({
    map: tex, transparent: true, depthWrite: false,
    // Render after opaque scene content so halos blend over columns/lines.
    depthTest: true,
  });
  const sprite = new THREE.Sprite(mat);
  sprite.scale.set(height * (CANVAS_W / CANVAS_H), height, 1);
  /* CRITICAL: Sprite.raycast() THROWS unless the raycaster has a camera
   * set — and the XR rig's controller-beam raycast (xrRig.update) sweeps
   * the whole scene with a plain ray, no camera. The first visible label
   * sprite would kill the render loop in-headset (frozen frame that
   * follows the head — looks "camera-locked"). Labels are annotation, not
   * targets: opt out of raycasting entirely. */
  sprite.raycast = () => {};

  let current: string | null = null;
  function setText(next: string) {
    if (next === current) return;
    current = next;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.clearRect(0, 0, CANVAS_W, CANVAS_H);
    ctx.font = `${fontPx}px system-ui, "Hiragino Sans", "Noto Sans JP", sans-serif`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    // Soft halo box behind the text — keeps kanji legible over bright
    // passthrough or a same-hue column directly below.
    const w = Math.min(CANVAS_W - 4, ctx.measureText(next).width + 18);
    ctx.fillStyle = haloColor;
    if (typeof ctx.roundRect === 'function') {
      ctx.beginPath();
      ctx.roundRect((CANVAS_W - w) / 2, 6, w, CANVAS_H - 12, 10);
      ctx.fill();
    } else {
      // Older embedded browsers (Spectacles-class) lack roundRect.
      ctx.fillRect((CANVAS_W - w) / 2, 6, w, CANVAS_H - 12);
    }
    ctx.fillStyle = color;
    ctx.fillText(next, CANVAS_W / 2, CANVAS_H / 2 + 1);
    tex.needsUpdate = true;
  }
  if (text) setText(text);

  return {
    sprite,
    setText,
    dispose() {
      tex.dispose();
      mat.dispose();
    },
  };
}
