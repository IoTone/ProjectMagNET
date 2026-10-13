import { describe, it, expect, afterEach } from 'vitest';
import { isSpectacles, isQuest, isAndroidXR, platformName, _setUaForTest } from './platform';

afterEach(() => _setUaForTest(null));

/* Sample UA strings — sourced from public docs + community reports. Tighten
 * once we've captured real UAs from each device via remote-inspect. */
const QUEST3_UA =
  'Mozilla/5.0 (X11; Linux x86_64; Quest 3) AppleWebKit/537.36 (KHTML, like Gecko) ' +
  'OculusBrowser/30.0 Chrome/120.0.0.0 VR Safari/537.36';
const SPECTACLES_UA =
  'Mozilla/5.0 (Linux; Spectacles 5.2) AppleWebKit/605.1.15 (KHTML, like Gecko) ' +
  'Version/16.0 Mobile/15E148 Safari/604.1';
// Reference Android XR device: Chrome 138 on Android 10 (no OculusBrowser).
const ANDROIDXR_UA =
  'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) ' +
  'Chrome/138.0.0.0 Mobile Safari/537.36';
const DESKTOP_UA =
  'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) ' +
  'Version/17.0 Safari/605.1.15';

describe('isSpectacles', () => {
  it('matches a Spectacles UA', () => {
    _setUaForTest(SPECTACLES_UA);
    expect(isSpectacles()).toBe(true);
  });

  it('does NOT match Quest 3', () => {
    _setUaForTest(QUEST3_UA);
    expect(isSpectacles()).toBe(false);
  });

  it('does NOT match desktop Safari', () => {
    _setUaForTest(DESKTOP_UA);
    expect(isSpectacles()).toBe(false);
  });

  it('is case-insensitive', () => {
    _setUaForTest('Some Future Build / SPECTACLES Rev 7');
    expect(isSpectacles()).toBe(true);
  });

  it('handles empty UA gracefully', () => {
    _setUaForTest('');
    expect(isSpectacles()).toBe(false);
  });
});

describe('isQuest', () => {
  it('matches a Quest 3 UA', () => {
    _setUaForTest(QUEST3_UA);
    expect(isQuest()).toBe(true);
  });

  it('does NOT match Spectacles', () => {
    _setUaForTest(SPECTACLES_UA);
    expect(isQuest()).toBe(false);
  });

  it('requires BOTH OculusBrowser AND Quest tokens (no false positive on Go-era UA)', () => {
    // Old Oculus Browser on a non-Quest device — should not satisfy isQuest.
    _setUaForTest(
      'Mozilla/5.0 (X11; Linux x86_64; Oculus Go) AppleWebKit/537.36 OculusBrowser/8.0',
    );
    expect(isQuest()).toBe(false);
  });

  it('does NOT match desktop', () => {
    _setUaForTest(DESKTOP_UA);
    expect(isQuest()).toBe(false);
  });
});

describe('isAndroidXR', () => {
  it('matches the Chrome-138-on-Android-10 reference UA', () => {
    _setUaForTest(ANDROIDXR_UA);
    expect(isAndroidXR()).toBe(true);
  });

  it('does NOT match Quest 3 (Chromium on Android-ish, but OculusBrowser)', () => {
    _setUaForTest(QUEST3_UA);
    expect(isAndroidXR()).toBe(false);
  });

  it('does NOT match Spectacles (WebKit, not Chromium)', () => {
    _setUaForTest(SPECTACLES_UA);
    expect(isAndroidXR()).toBe(false);
  });

  it('does NOT match desktop Safari', () => {
    _setUaForTest(DESKTOP_UA);
    expect(isAndroidXR()).toBe(false);
  });

  it('handles empty UA gracefully', () => {
    _setUaForTest('');
    expect(isAndroidXR()).toBe(false);
  });
});

describe('platformName', () => {
  it.each([
    [SPECTACLES_UA, 'spectacles'],
    [QUEST3_UA,     'quest'],
    [ANDROIDXR_UA,  'androidxr'],
    [DESKTOP_UA,    'desktop'],
    ['',            'desktop'],
  ])('UA %s → %s', (uaStr, expected) => {
    _setUaForTest(uaStr);
    expect(platformName()).toBe(expected);
  });

  it('prefers spectacles over quest if both tokens somehow appear', () => {
    // Defensive — shouldn't happen in practice but the detector ordering
    // matters if some future stub UA contains both.
    _setUaForTest('Mozilla/5.0 (Spectacles) OculusBrowser Quest');
    expect(platformName()).toBe('spectacles');
  });
});

describe('_setUaForTest', () => {
  it('null restores real navigator.userAgent (no override)', () => {
    _setUaForTest('Mozilla/5.0 (Spectacles)');
    expect(isSpectacles()).toBe(true);
    _setUaForTest(null);
    // Once cleared, falls back to whatever the test env reports. We don't
    // assert what that is — only that the override is no longer in force.
    expect(isSpectacles()).toBe(false);
  });
});
