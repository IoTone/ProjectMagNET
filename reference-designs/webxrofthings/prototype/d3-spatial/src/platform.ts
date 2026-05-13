/**
 * XR platform detection.
 *
 * Use these helpers to gate renderer settings, feature requests, or perf
 * trade-offs that differ between Quest 3 and Snap Spectacles. The two
 * platforms disagree on what WebXR/WebGL settings they tolerate — most
 * notably, Spectacles' runtime blanks the XR scene if
 * `renderer.xr.setFramebufferScaleFactor` is set to anything other than 1.0,
 * while Quest 3 honors values down to ~0.7 as a foveation hint (see
 * memory `project_spectacles_webxr_renderer_quirks.md`).
 *
 * Detection is UA-based today. That's good enough for the platforms we
 * actively ship to; it's not airtight against future browser-engine
 * changes. If we ever start sniffing the wrong thing, the right next move
 * is feature-detect via `navigator.xr.isSessionSupported(...)` rather than
 * sprinkling more UA-substring checks.
 *
 * Empirical UA tokens (verify on-device and tighten if needed):
 *   - Snap Spectacles : userAgent contains "Spectacles"
 *   - Meta Quest 3    : userAgent contains "OculusBrowser" and "Quest"
 *
 * To capture the actual UA from a device, drop a one-time
 * `console.log(navigator.userAgent)` near the renderer setup and read it
 * back via remote-inspect.
 */

/** Test-only override of the UA the detectors below see. `null` = use the
 *  real `navigator.userAgent`. Exposed via `_setUaForTest`. */
let _uaOverride: string | null = null;

/** Read the active UA: test override if set, else `navigator.userAgent`,
 *  else empty string (non-browser contexts). */
function ua(): string {
  if (_uaOverride !== null) return _uaOverride;
  if (typeof navigator === 'undefined') return '';
  return navigator.userAgent ?? '';
}

/**
 * `true` when running inside Snap Spectacles' WebXR browser.
 *
 * Gate any of these behind `!isSpectacles()` to keep the platform happy:
 *   - `renderer.xr.setFramebufferScaleFactor(scale)` where `scale !== 1.0`
 *   - constructor flag `antialias: false` (suspected; reverted defensively)
 */
export function isSpectacles(): boolean {
  return /Spectacles/i.test(ua());
}

/**
 * `true` when running inside Meta Quest's OculusBrowser (Quest 3 family).
 * Both tokens must be present to avoid false positives on older Oculus
 * Browser builds that ran on Go / Quest 1 with different perf profiles.
 */
export function isQuest(): boolean {
  const s = ua();
  return /OculusBrowser/i.test(s) && /Quest/i.test(s);
}

/**
 * Short name useful for debug HUDs. Returns 'desktop' for any non-XR-
 * recognised UA (the smoke harness, dev preview, etc.).
 */
export function platformName(): 'spectacles' | 'quest' | 'desktop' {
  if (isSpectacles()) return 'spectacles';
  if (isQuest())      return 'quest';
  return 'desktop';
}

/** Test-only override of the UA the detectors see. Pass `null` to restore
 *  real `navigator.userAgent`. */
export function _setUaForTest(s: string | null): void { _uaOverride = s; }
