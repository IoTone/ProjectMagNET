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
 * ⚠️ Spectacles masquerades. Its browser reports a generic
 * `Mozilla ... Linux aarch64 ... KHTML` UA with no "Spectacles" token,
 * presumably to maximize web compatibility. That means **`isSpectacles()`
 * returns `false` on real Spectacles devices today** — UA-sniffing simply
 * can't tell. Until we have a better signal, code that needs to be safe
 * for Spectacles must use **fail-safe gating** — i.e. opt *in* to perf
 * tweaks only when a known-good platform is positively detected:
 *
 *     // GOOD — Spectacles falls through to the safe default
 *     if (isQuest()) renderer.xr.setFramebufferScaleFactor(0.9);
 *
 *     // BAD — Spectacles' real UA doesn't match, so this fires anyway
 *     if (!isSpectacles()) renderer.xr.setFramebufferScaleFactor(0.9);
 *
 * **Better detection — future work.** UA-sniffing is wrong; the right
 * primitives are runtime: after `sessionstart` we can inspect
 * `session.inputSources`, `session.supportedFrameRates`, available
 * features (`bounded-floor` differs between the platforms), or render the
 * first frame and probe the framebuffer dimensions to spot a malformed
 * XR layer. Open as TODO — see also the memory note for the platform's
 * blank-on-non-1.0 framebuffer-scale behaviour.
 *
 * Empirical UA tokens used by the regex below (kept in case a future
 * Spectacles build stops masquerading):
 *   - Snap Spectacles : userAgent contains "Spectacles"  (NOT TRIPPED TODAY)
 *   - Meta Quest 3    : userAgent contains "OculusBrowser" and "Quest"
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
 * `true` when the UA contains "Spectacles".
 *
 * **Reliably returns `false` on real Spectacles devices today** because
 * the browser masquerades — see the file header. Do NOT use this to
 * fail-permissive-gate perf tweaks; only call sites where a `true`
 * result is informational (e.g. logging) are safe.
 *
 * Kept as a stub for the day a future Spectacles build stops masquerading
 * and so callers don't need to invent their own UA regex when that
 * happens.
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
