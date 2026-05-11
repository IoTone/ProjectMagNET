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
 * Caveat: the bundled Roboto-msdf only covers ~223 chars (Basic Latin + a slice
 * of Latin-1). Glyphs outside the set (middle dot, em dash, arrows, emoji)
 * throw inside three-mesh-ui's MSDFGlyph constructor and take down the scene.
 * `sanitizeText()` below maps the common offenders to ASCII fallbacks; pass
 * dynamic strings (manifest labels, status messages, etc.) through it before
 * handing them to ThreeMeshUI.Text.
 *
 * Mark/chart labels (in src/viz/**) still use troika because their text lives
 * on bare THREE.Group hierarchies, not inside Blocks. A future pass can either
 * wrap those in Blocks or tune troika's sdfGlyphSize/outlineWidth on the
 * viz layer specifically.
 */
import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';

/** Public-served paths — Vite copies anything under `public/` to the dist root. */
export const FONT_FAMILY_URL = '/fonts/Roboto-msdf.json';
export const FONT_TEXTURE_URL = '/fonts/Roboto-msdf.png';

/**
 * Block options every chrome Block should spread in so its Text children
 * find the MSDF atlas. Stripped of layout opinions on purpose — each caller
 * still owns its own width/height/justifyContent/etc.
 *
 * Apply it on *every* Block, not just roots: three-mesh-ui's font lookup
 * walks up the `parentUI` chain, but defensive declaration is cheaper than
 * debugging "no font was found" warnings on intermediate slot Blocks.
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

/**
 * Map of unsupported-but-common characters to ASCII fallbacks. Anything
 * outside Basic Latin / Latin-1 that *isn't* in this map gets stripped by
 * `sanitizeText`. Keep entries here when adding new strings — if you see a
 * unicode glyph that disappears in the UI, add a replacement.
 */
const CHAR_FALLBACKS: Record<string, string> = {
  '·': '-',      // middle dot ·
  '—': '--',     // em dash —
  '–': '-',      // en dash –
  '‘': "'",      // left single quote '
  '’': "'",      // right single quote '
  '“': '"',      // left double quote "
  '”': '"',      // right double quote "
  '•': '*',      // bullet •
  '…': '...',    // ellipsis …
  '→': '->',     // right arrow →
  '←': '<-',     // left arrow ←
  '▲': '^',      // up triangle ▲
  '▼': 'v',      // down triangle ▼
  '✓': '+',      // check ✓
  '✗': 'x',      // cross ✗
  '×': 'x',      // multiplication sign ×
};

/**
 * Replace or strip characters not in the bundled Roboto MSDF atlas. Pass any
 * dynamic string through this before handing it to `new ThreeMeshUI.Text` or
 * `.set({ content })`.
 *
 * Static strings inside this codebase should be written with ASCII directly
 * (avoid sneaking middle dots and em dashes into source); this helper exists
 * for content that comes from manifests / device names / user-typed paths
 * where we can't control the input.
 */
export function sanitizeText(s: string): string {
  let out = '';
  for (const ch of s) {
    const code = ch.codePointAt(0) ?? 0;
    if (code < 0x80) {                  // ASCII — fast path, the common case
      out += ch;
    } else if (CHAR_FALLBACKS[ch] !== undefined) {
      out += CHAR_FALLBACKS[ch];
    } else if (code <= 0xff) {          // Latin-1 — the MSDF mostly covers this
      out += ch;
    } else {
      // Outside Latin-1 with no fallback — drop it rather than crash MSDFGlyph.
      // If we keep finding gaps, the long-term fix is a richer custom MSDF.
    }
  }
  return out;
}

/**
 * Invisible transparent Block sized to a specific slot in the parent's
 * absolute layout — convenient when porting troika-positioned text to MSDF.
 * Each slot is a fully-configured Block (with the font attached) so its
 * Text child doesn't depend on font inheritance walking through an ancestor.
 */
export function makeSlot(
  width: number,
  height: number,
  opts: { align?: 'center' | 'start' | 'end'; textAlign?: 'center' | 'left' | 'right' } = {},
): InstanceType<typeof ThreeMeshUI.Block> {
  return new ThreeMeshUI.Block({
    ...FONT_BLOCK_OPTS,
    width, height,
    backgroundOpacity: 0,
    borderOpacity: 0,
    justifyContent: 'center',
    alignItems: opts.align ?? 'center',
    textAlign: opts.textAlign ?? 'center',
  });
}
