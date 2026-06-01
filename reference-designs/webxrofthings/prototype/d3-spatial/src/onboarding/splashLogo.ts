/**
 * splashLogo — "Stranger Things"-style glowing emblem reading
 * "THE WEBXR OF THINGS", shown above the join panel on the splash
 * screen and any time the user leaves a dataspace.
 *
 * Visual language (from the source reference):
 *   - HOLLOW red letterforms — no fill, only an outer + inner stroke.
 *   - Faint red halo around each glyph (outerGlow).
 *   - Four thin horizontal serif rules: above the top line, between
 *     the two lines (× 2 — one tight to top, one tight to bottom),
 *     and below the bottom line.
 *   - Black background; the logo reads at any z-depth thanks to the
 *     hollow strokes + halo, no fill plane required.
 *
 * Lifecycle:
 *   show()      → opacity ramps 0 → 1 over ~600 ms, pulse begins
 *   hide()      → opacity ramps current → 0 over ~400 ms, pulse stops
 *   The pulse is a slow sine modulating opacity 0.78 ↔ 1.00 over
 *   ~2.4 s. Driven from a tick group registered on the camera so it
 *   keeps running even when the panel mesh is in/out of frustum.
 *
 * troika-three-text doesn't render hollow letterforms natively, but
 * with `color: transparent` (fill alpha 0) + a thick `outlineWidth`
 * + `outlineColor` red, it produces the exact "neon outline" effect.
 * The halo comes from `outlineBlur` set to ~10-15 px equivalent —
 * troika rasterises the blurred outline into the SDF atlas.
 */

import * as THREE from 'three';
import { Text } from 'troika-three-text';

const RED        = 0xff1a1a;
const GLOW_RED   = 0xff4444;
const SERIF_RED  = 0xff2a2a;
/* Doubled vs the first pass per user feedback ("twice as slow") —
 * the slow ramp + the slow slide together read as "fade in quite
 * slowly from beyond the horizon." Pulse runs throughout, not only
 * after the fade-in finishes. */
const FADE_IN_MS         = 6400;
const FADE_OUT_MS        = 800;
const PULSE_PERIOD_MS    = 2400;
const PULSE_MIN_OPACITY  = 0.78;
const PULSE_MAX_OPACITY  = 1.00;

/* Approach geometry. The logo's WORLD position is computed per-frame
 * from the camera + its horizontal forward direction (NOT from the
 * join panel's matrix). Using the panel as parent introduced a slight
 * downward tilt — the panel sits 0.05 m below eye and its lookAt
 * mixes Y into the local Z basis, so the logo at panel-local Z=-8
 * landed ~6 cm BELOW eye, reading as "from below the join screen."
 * Computing in pure world coords with a horizontal-only forward
 * eliminates that mixing — the logo always travels along the eye-
 * level plane regardless of head pose.
 *
 *   RESTING_DISTANCE  = where the logo settles (matches the panel)
 *   APPROACH_DISTANCE = how far back the logo starts (~10 m total
 *                       from camera) — small enough at perspective
 *                       distance to read as "on the horizon"
 *   Y_OFFSET          = height above the camera's eye-line. ~0.30 m
 *                       puts the logo clearly above the panel's
 *                       title row. */
const RESTING_DISTANCE   = 1.2;
const APPROACH_DISTANCE  = 9.0;
const Y_OFFSET           = 0.30;
const SLIDE_DURATION_MS  = 7000;

export interface SplashLogo {
  group: THREE.Group;
  /** Begin fade-in + start the pulse. Idempotent. */
  show(): void;
  /** Begin fade-out; stops the pulse on completion. Idempotent. */
  hide(): void;
  /** Tick the fade + pulse + approach animation. Driven from the
   *  render loop. Pass the active camera so the logo can compute its
   *  own world position along the camera's forward sight-line — the
   *  caller doesn't (and shouldn't) set group.position externally. */
  tick(camera: THREE.Camera): void;
  dispose(): void;
}

/* troika-three-text exposes `fillOpacity`, `outlineWidth`,
 * `outlineColor`, `outlineOpacity`, `outlineBlur`, `strokeColor`,
 * `strokeWidth`, `strokeOpacity` at runtime, but its current TS
 * defs only declare the basics. This cast surface gives us the
 * extra knobs without `any` everywhere. */
type TroikaTextExtras = {
  fillOpacity?:    number;
  /* troika accepts either a number (= world units, same as fontSize)
   * OR a `'5%'`-style string (relative to fontSize). The number form
   * is a footgun — at fontSize 0.11 m, `outlineWidth: 0.13` is a
   * 130 mm wide stroke, hence the giant red blob in the first pass.
   * We use the string form everywhere. */
  outlineWidth?:   number | string;
  outlineColor?:   number | string;
  outlineOpacity?: number;
  outlineBlur?:    number | string;
  strokeColor?:    number | string;
  strokeWidth?:    number | string;
  strokeOpacity?:  number;
};

export function buildSplashLogo(): SplashLogo {
  const group = new THREE.Group();
  group.name = 'splash-logo';
  group.visible = false;

  /* Two-line layout:
   *   THE  WEBXR  OF      ← upper line, slightly smaller
   *      ─────────────    ← serif rule below upper
   *      ─────────────    ← serif rule above lower
   *        THINGS         ← lower line, larger, dominant
   *      ─────────────    ← serif rule below lower
   *
   * The visual weight in the reference logo sits on the second
   * line (Stranger / Things split). Mirroring that hierarchy keeps
   * the read order intuitive in XR. */

  const UPPER_SIZE   = 0.062;
  const LOWER_SIZE   = 0.110;
  const UPPER_Y      =  0.075;
  const LOWER_Y      = -0.070;
  const RULE_HALF_W  = 0.320;
  const RULE_THICK   = 0.0035;

  /* Helper: build a troika Text with the hollow-outline style. The
   * fill (`color`) is set black (would be transparent but troika
   * doesn't actually let you set fill alpha independent of outline
   * alpha, so a dark fill that's the same as the scene bg gets us
   * the same visual). `outlineWidth` is a font-size-relative number;
   * 0.13 reads as a clean 4-5 px stroke in the SDF atlas. */
  function buildHollowText(t: string, size: number): Text {
    const txt = new Text();
    txt.text     = t;
    txt.fontSize = size;
    txt.color    = 0x000000;     // appears as 'no fill' against the dark scene bg
    txt.anchorX  = 'center';
    txt.anchorY  = 'middle';
    const ex = txt as unknown as TroikaTextExtras;
    ex.fillOpacity    = 0;
    /* String '%' values are interpreted relative to fontSize.
     * Reference logo glyph ratios: ~3% outline stroke + ~5% soft
     * outline halo, plus a wider ~10% outer glow at low opacity. */
    ex.outlineWidth   = '3%';
    ex.outlineColor   = RED;
    ex.outlineOpacity = 1.0;
    ex.outlineBlur    = '5%';
    /* Outer neon-bleed halo. Wider stroke + low opacity reads as a
     * gradient haze around each glyph without obscuring it. */
    ex.strokeColor    = GLOW_RED;
    ex.strokeWidth    = '10%';
    ex.strokeOpacity  = 0.35;
    txt.sync();
    return txt;
  }

  /* Mixed case reads as the project's actual branding ("The WebXR
   * of Things") rather than ALL-CAPS, which felt shoutier than the
   * Stranger-Things reference required. troika's default Roboto
   * doesn't auto-transform case; uppercase was a string choice, not
   * a font behaviour. */
  const upper = buildHollowText('The WebXR of', UPPER_SIZE);
  upper.position.set(0, UPPER_Y, 0);
  group.add(upper);

  const lower = buildHollowText('Things', LOWER_SIZE);
  lower.position.set(0, LOWER_Y, 0);
  group.add(lower);

  /* Horizontal serif rules. Built as thin red planes (LineBasicMaterial
   * is hard to thicken cross-platform on Spectacles, so a flat
   * BoxGeometry / PlaneGeometry slab is more reliable). */
  const ruleMat = new THREE.MeshBasicMaterial({
    color: SERIF_RED, transparent: true, opacity: 1.0, depthWrite: false,
  });
  const ruleGeo = new THREE.PlaneGeometry(RULE_HALF_W * 2, RULE_THICK);
  function addRule(y: number): THREE.Mesh {
    const r = new THREE.Mesh(ruleGeo, ruleMat);
    r.position.set(0, y, 0);
    group.add(r);
    return r;
  }
  const ruleTop      = addRule(UPPER_Y + UPPER_SIZE * 0.65);
  const ruleBetween1 = addRule(UPPER_Y - UPPER_SIZE * 0.65);
  const ruleBetween2 = addRule(LOWER_Y + LOWER_SIZE * 0.55);
  const ruleBottom   = addRule(LOWER_Y - LOWER_SIZE * 0.55);

  /* ─── Fade + pulse state ─────────────────────────────────────── */
  type FadeState = 'hidden' | 'fading-in' | 'visible' | 'fading-out';
  let fadeState: FadeState = 'hidden';
  let fadeStartMs = 0;
  let fadeFromOpacity = 0;
  let currentOpacity = 0;

  /* Slide state. slideStartMs > 0 while the approach animation is
   * running; tick() advances group.position.z each frame and clears
   * it back to 0 on completion. The caller's `position.set(x, y, z)`
   * still works — only `position.z` is overridden during the slide. */
  let slideStartMs = 0;

  function setOpacity(o: number) {
    currentOpacity = o;
    /* troika Text honours fillOpacity + outlineOpacity + strokeOpacity
     * independently of THREE.Material.opacity. Hit all three so the
     * fade looks uniform. The serif rules ride the standard
     * material.opacity path. */
    const eu = upper as unknown as TroikaTextExtras;
    const el = lower as unknown as TroikaTextExtras;
    eu.outlineOpacity = o;
    el.outlineOpacity = o;
    /* Stroke is the soft halo — base 0.35 (matches buildHollowText),
     * scaled by overall opacity so fade-out hides both layers
     * proportionally. */
    eu.strokeOpacity  = 0.35 * o;
    el.strokeOpacity  = 0.35 * o;
    ruleMat.opacity   = o;
    /* sync() re-renders the SDF text with the updated opacity
     * uniforms — without it the change is silent. */
    upper.sync();
    lower.sync();
  }

  function show() {
    if (fadeState === 'visible' || fadeState === 'fading-in') return;
    group.visible = true;
    fadeState = 'fading-in';
    fadeStartMs = performance.now();
    fadeFromOpacity = currentOpacity;
    /* Reset the approach so every show() (initial splash + every
     * leave-dataspace re-entry) starts the logo at the far end. */
    slideStartMs = performance.now();
  }

  function hide() {
    if (fadeState === 'hidden' || fadeState === 'fading-out') return;
    fadeState = 'fading-out';
    fadeStartMs = performance.now();
    fadeFromOpacity = currentOpacity;
  }

  /* Scratch vectors — reused each frame so the per-frame tick
   * doesn't allocate. */
  const _camPos    = new THREE.Vector3();
  const _camFwd    = new THREE.Vector3();
  const _logoWorld = new THREE.Vector3();
  const _lookTgt   = new THREE.Vector3();

  function placeAlongSightLine(camera: THREE.Camera, distance: number) {
    /* Camera world position + horizontal forward (Y stripped) so
     * the logo travels along the eye-level plane regardless of how
     * the user has their head tilted. */
    camera.getWorldPosition(_camPos);
    _camFwd.set(0, 0, -1).applyQuaternion(camera.quaternion);
    _camFwd.y = 0;
    if (_camFwd.lengthSq() < 1e-4) _camFwd.set(0, 0, -1);
    _camFwd.normalize();

    _logoWorld.set(
      _camPos.x + _camFwd.x * distance,
      _camPos.y + Y_OFFSET,
      _camPos.z + _camFwd.z * distance,
    );
    group.position.copy(_logoWorld);
    /* lookAt the camera at the logo's own Y. Same-Y target keeps the
     * up vector pure +Y → no tilt mixing into the local axes, the
     * logo stays squarely facing the user. */
    _lookTgt.set(_camPos.x, _logoWorld.y, _camPos.z);
    group.lookAt(_lookTgt);
  }

  function tick(camera: THREE.Camera) {
    const now = performance.now();

    /* Approach: distance from camera shrinks from
     * RESTING_DISTANCE + APPROACH_DISTANCE → RESTING_DISTANCE over
     * SLIDE_DURATION_MS with easeOutCubic. Runs independently of
     * the fade state so the logo glides in while the opacity ramp
     * + pulse animate at their own cadences. */
    let distance = RESTING_DISTANCE;
    if (slideStartMs > 0) {
      const t = Math.min(1, (now - slideStartMs) / SLIDE_DURATION_MS);
      const eased = 1 - Math.pow(1 - t, 3);
      distance = RESTING_DISTANCE + APPROACH_DISTANCE * (1 - eased);
      if (t >= 1) slideStartMs = 0;
    }
    placeAlongSightLine(camera, distance);

    /* Pulse runs ALL THE TIME the logo is visible — including during
     * the fade-in slide, per user feedback ("pulsing as it moves").
     * Compute the pulse multiplier first, then multiply by whatever
     * the fade-state envelope demands. */
    const phase = (now % PULSE_PERIOD_MS) / PULSE_PERIOD_MS;
    const pulseSin = (Math.sin(phase * 2 * Math.PI) + 1) * 0.5;        // 0..1
    const pulseAmp = PULSE_MIN_OPACITY + (PULSE_MAX_OPACITY - PULSE_MIN_OPACITY) * pulseSin;

    if (fadeState === 'fading-in') {
      const t = Math.min(1, (now - fadeStartMs) / FADE_IN_MS);
      /* Envelope from fadeFromOpacity → 1.0, multiplied by the
       * pulse amplitude so the user sees the pulse rhythm bleed
       * through the ramp. */
      const envelope = fadeFromOpacity + (1.0 - fadeFromOpacity) * t;
      setOpacity(envelope * pulseAmp);
      if (t >= 1) fadeState = 'visible';
      return;
    }
    if (fadeState === 'fading-out') {
      const t = Math.min(1, (now - fadeStartMs) / FADE_OUT_MS);
      setOpacity(fadeFromOpacity * (1 - t));
      if (t >= 1) { fadeState = 'hidden'; group.visible = false; }
      return;
    }
    if (fadeState === 'visible') {
      setOpacity(pulseAmp);
    }
  }

  function dispose() {
    (upper as unknown as { dispose?: () => void }).dispose?.();
    (lower as unknown as { dispose?: () => void }).dispose?.();
    ruleGeo.dispose();
    ruleMat.dispose();
  }

  /* Initial opacity is 0; first show() begins the ramp. */
  setOpacity(0);

  return { group, show, hide, tick, dispose };
}
