/**
 * Live spatial-audio cell — small boombox mesh + a `THREE.PositionalAudio`
 * source playing a procedural music loop. UC4's "music player" in P5b.
 *
 * Why this is more than a mark builder for an audio file:
 *   - The procedural buffer ships as code, not an asset — no MP3 to license,
 *     no bundle bloat.
 *   - `PositionalAudio` spatialises the loop relative to the camera's
 *     `AudioListener`, so the user can walk around the boombox in AR and
 *     hear stereo panning + distance attenuation.
 *
 * The cell needs the scene's AudioListener (attached to the camera) to
 * spatialise correctly. The manifest pipeline doesn't currently plumb a
 * builder context, so we consume the listener from the `__demo` bridge
 * that `main.ts` already exposes for the smoke harness. If the bridge
 * isn't available (tests, headless contexts), the cell skips audio init
 * and the visualisation still renders.
 *
 * Autoplay caveat: WebAudio playback requires a user gesture. The join-code
 * submit IS such a gesture, so calling `play()` shortly after dataspace
 * load typically works. If we hit a NotAllowedError, we defer to the next
 * pointer/key event and try again.
 */

import * as THREE from 'three';
import { makeMusicLoopBuffer } from '../audio/proceduralMusic';
import { TEXT } from '../ui/palette';

export interface LiveSpatialAudioOpts {
  /** Half-length of the boombox body in world units. Default 0.08. */
  size?: number;
  /** Body colour (hex). Default warm muted. */
  bodyColor?: number;
  /** Speaker / accent colour. Default warm primary. */
  accentColor?: number;
  /** Reference distance for the positional rolloff. Default 0.6. */
  refDistance?: number;
  /** Initial gain (0..1). Default 0.7. */
  gain?: number;
  /** If true, attempt to start playing on construction (subject to
   *  autoplay policy). Default true. */
  autoplay?: boolean;
}

export interface LiveSpatialAudioCell {
  group: THREE.Group;
  play(): Promise<void>;
  pause(): void;
  isPlaying(): boolean;
  setGain(g: number): void;
  tick(time: number): void;
  dispose(): void;
}

/** Read the scene's AudioListener from `window.__demo.audioListener` if
 *  present. Returns null when running in node tests or before main.ts
 *  has finished setting up. */
function findSceneListener(): THREE.AudioListener | null {
  if (typeof window === 'undefined') return null;
  const bridge = (window as unknown as { __demo?: { audioListener?: THREE.AudioListener } }).__demo;
  return bridge?.audioListener ?? null;
}

export function buildLiveSpatialAudioCell(opts: LiveSpatialAudioOpts = {}): LiveSpatialAudioCell {
  const {
    size = 0.08,
    bodyColor = TEXT.muted,
    accentColor = TEXT.primary,
    refDistance = 0.6,
    gain = 0.7,
    autoplay = true,
  } = opts;

  const group = new THREE.Group();
  group.name = 'live-spatial-audio';

  // ─── Boombox mesh ─────────────────────────────────────────────────
  // Simple shapes — no GLTF dependency, no asset to ship.
  const bodyMat   = new THREE.MeshBasicMaterial({ color: bodyColor });
  const accentMat = new THREE.MeshBasicMaterial({ color: accentColor });

  // Main body — rectangular, wider than tall (think portable speaker).
  const body = new THREE.Mesh(
    new THREE.BoxGeometry(size * 2.4, size * 1.3, size * 0.9),
    bodyMat,
  );
  group.add(body);

  // Two speaker cones on the front face. Slight inset using z offset.
  const speakerGeo = new THREE.CylinderGeometry(size * 0.42, size * 0.42, size * 0.06, 24);
  const speakerL = new THREE.Mesh(speakerGeo, accentMat);
  speakerL.rotation.x = Math.PI / 2;
  speakerL.position.set(-size * 0.7, 0, size * 0.5);
  group.add(speakerL);
  const speakerR = new THREE.Mesh(speakerGeo, accentMat);
  speakerR.rotation.x = Math.PI / 2;
  speakerR.position.set( size * 0.7, 0, size * 0.5);
  group.add(speakerR);

  // Handle on top — small bar.
  const handle = new THREE.Mesh(
    new THREE.BoxGeometry(size * 1.0, size * 0.08, size * 0.12),
    accentMat,
  );
  handle.position.set(0, size * 0.75, 0);
  group.add(handle);

  // Tiny "playing" LED dot in the centre-front, emissive-bright when on.
  const ledMat = new THREE.MeshBasicMaterial({ color: 0x2a2520 });   // off
  const led = new THREE.Mesh(new THREE.SphereGeometry(size * 0.05, 12, 8), ledMat);
  led.position.set(0, -size * 0.35, size * 0.46);
  group.add(led);

  // ─── Audio source ─────────────────────────────────────────────────
  const listener = findSceneListener();
  let positional: THREE.PositionalAudio | null = null;
  let playing = false;
  let pendingGesture = false;

  function tryPlayDeferred() {
    if (!positional || !pendingGesture) return;
    pendingGesture = false;
    window.removeEventListener('pointerdown', tryPlayDeferred);
    window.removeEventListener('keydown', tryPlayDeferred);
    // Now that we have a gesture, retry the play path — this resumes the
    // AudioContext if needed and starts the source.
    void play();
  }

  if (listener) {
    positional = new THREE.PositionalAudio(listener);
    positional.setRefDistance(refDistance);
    positional.setRolloffFactor(1.0);
    positional.setDistanceModel('inverse');
    positional.setLoop(true);
    positional.setVolume(gain);
    positional.setBuffer(makeMusicLoopBuffer(listener.context));
    body.add(positional);
  }

  /**
   * Start playing. Web Audio's autoplay policy blocks playback until the
   * page has received a user gesture; calling `PositionalAudio.play()`
   * while the context is suspended does not throw (the browser just logs
   * a console warning), so we have to check `ctx.state` ourselves rather
   * than relying on try/catch.
   *
   * If the context is suspended we register one-shot pointer/key listeners
   * — the first input gesture invokes `tryPlayDeferred`, which retries
   * play(). At that point `ctx.resume()` will succeed because we're in
   * a gesture-handler context.
   */
  async function play(): Promise<void> {
    if (!positional || !listener) return;
    const ctx = listener.context;

    // Try to bring the context out of suspended state. This only succeeds
    // when we're running inside a user-gesture event handler — outside one
    // the promise resolves but the state stays 'suspended'.
    if (ctx.state === 'suspended') {
      try { await ctx.resume(); } catch { /* defer below */ }
    }

    if (ctx.state !== 'running') {
      // Still suspended — register the one-shot gesture listener so the
      // next click/keypress resumes us. Idempotent across repeat play()
      // attempts because we guard with `pendingGesture`.
      if (!pendingGesture && typeof window !== 'undefined') {
        pendingGesture = true;
        window.addEventListener('pointerdown', tryPlayDeferred, { once: true });
        window.addEventListener('keydown', tryPlayDeferred, { once: true });
      }
      return;
    }

    if (playing) return;
    positional.play();
    playing = true;
    ledMat.color.setHex(accentColor);
  }

  function pause() {
    if (!positional || !playing) return;
    positional.pause();
    playing = false;
    ledMat.color.setHex(0x2a2520);
  }

  function setGain(g: number) {
    if (!positional) return;
    positional.setVolume(Math.max(0, Math.min(1, g)));
  }

  if (autoplay && positional) {
    // Run play() through the gesture-deferral path so an autoplay-block
    // doesn't show as an unhandled rejection. We swallow the rejection
    // because the deferred listeners take care of the retry.
    play().catch(() => { /* deferred via tryPlayDeferred */ });
  }

  return {
    group,
    play,
    pause,
    isPlaying: () => playing,
    setGain,
    tick: (_time: number) => { /* nothing per-frame; PositionalAudio
                                  auto-spatialises from group transform */ },
    dispose: () => {
      if (positional) {
        if (playing) positional.stop();
        body.remove(positional);
      }
      if (typeof window !== 'undefined') {
        window.removeEventListener('pointerdown', tryPlayDeferred);
        window.removeEventListener('keydown', tryPlayDeferred);
      }
      body.geometry.dispose();
      speakerGeo.dispose();
      handle.geometry.dispose();
      led.geometry.dispose();
      bodyMat.dispose();
      accentMat.dispose();
      ledMat.dispose();
    },
  };
}
