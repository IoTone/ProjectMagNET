/**
 * Text palette for XR optical passthrough.
 *
 * Why not blue: optical waveguides (Spectacles, Quest passthrough) attenuate
 * short-wavelength light disproportionately, and the eye's luminance response
 * peaks near 555 nm (yellow-green). Cool blues read as faint and can be
 * unreadable over bright real-world backgrounds.
 *
 * All entries are tuned for high legibility on see-through displays.
 */
export const TEXT = {
    /** Primary titles, section headers, key callouts. Warm amber. */
    primary: 0xffd97a,
    /** Body copy, node labels, regular text. Warm cream. */
    body: 0xf5e9c8,
    /** Subtitles, unit labels, secondary info. Muted warm. */
    muted: 0xb8a380,
    /** Dim / disabled / off-state. */
    dim: 0x7a6e5a,
    /** Emphasis / value readouts. Pure luminance. */
    emphasis: 0xffffff,
    /** Success / active state. Soft mint. */
    accent: 0x88ff99,
    /** Warning / attention. Warm peach. */
    warn: 0xffb873,
    /** Error. Warm coral (not pure red). */
    error: 0xff7a8a,
};
/** Edge / connection line colors — must be visible on optical passthrough. */
export const EDGE = {
    /** Default link/edge color. Warm mid-tone, not dark blue. */
    link: 0xb8a380,
    /** Axis lines on charts. */
    axis: 0x9a8a70,
};
