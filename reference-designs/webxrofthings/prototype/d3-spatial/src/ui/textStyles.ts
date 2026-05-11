/**
 * Shared text styling for the chrome layer (Toolbar, PrivacyBanner, etc.).
 *
 * We use three-mesh-ui's MSDF text instead of troika-three-text for any text
 * rendered inside a ThreeMeshUI.Block, because:
 *   - MSDF gives sharp edges at every distance/scale/angle without per-instance
 *     tuning — the antialiasing demo at https://felixmariotto.github.io/three-mesh-ui/#antialiasing
 *     shows the same glyph readable from across the room and from 30 cm away.
 *   - Layout is parent-driven: setting `fontFamily`/`fontTexture` on a parent
 *     Block means every Text descendant inherits it, so we don't sprinkle the
 *     font URL through 7+ component files.
 *
 * Mark/chart labels (in src/viz/**) still use troika because their text lives
 * on bare THREE.Group hierarchies, not inside Blocks. A future pass can either
 * wrap those in Blocks or tune troika's sdfGlyphSize/outlineWidth on the
 * viz layer specifically.
 */
import * as THREE from 'three';

/** Public-served paths — Vite copies anything under `public/` to the dist root. */
export const FONT_FAMILY_URL = '/fonts/Roboto-msdf.json';
export const FONT_TEXTURE_URL = '/fonts/Roboto-msdf.png';

/**
 * Block options every chrome Block should spread in so its Text children
 * find the MSDF atlas. Stripped of layout opinions on purpose — each caller
 * still owns its own width/height/justifyContent/etc.
 */
export const FONT_BLOCK_OPTS = {
  fontFamily: FONT_FAMILY_URL,
  fontTexture: FONT_TEXTURE_URL,
} as const;

/**
 * Convert a `number` color (hex int from `palette.ts`) into the
 * `THREE.Color` instance ThreeMeshUI expects on `fontColor`.
 *
 * Inlined here rather than importing from three so callers can pass our
 * palette values directly without re-instantiating Color at every call site.
 */
export function fontColor(hex: number): THREE.Color {
  return new THREE.Color(hex);
}
