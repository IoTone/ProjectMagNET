/**
 * Live spatial-audio cell — UC4 cabin boombox.
 *
 * Renders a blue line-wireframe boombox mesh with explicit Play / Pause
 * icon meshes, a particle plume that animates while playing, and a
 * text label showing which procedural-music theme is loaded. The music
 * is procedurally generated (see `proceduralMusic.ts`) — no asset to
 * ship — and the cell picks a random theme on each construction so the
 * boombox feels different every time the user enters UC4.
 *
 * Audio is spatialised via THREE.PositionalAudio against the scene's
 * shared AudioListener, found through the `__demo` bridge that
 * `main.ts` exposes. The cell still renders its mesh when the
 * listener is unavailable (tests / pre-init); audio just doesn't play.
 *
 * Web Audio autoplay policy: contexts start suspended until the first
 * user gesture. `play()` calls `ctx.resume()` and, when the context
 * stays suspended, registers a one-shot pointer/key listener that
 * retries.
 */

import * as THREE from 'three';
import { Text } from 'troika-three-text';
import {
  makeMusicLoopBuffer,
  pickRandomTheme,
  MUSIC_THEME_LABELS,
  type MusicTheme,
} from '../audio/proceduralMusic';
import { TEXT } from '../ui/palette';

export interface LiveSpatialAudioOpts {
  /** Half-length of the boombox body in world units. Default 0.10. */
  size?: number;
  /** Wireframe / icon colour for the boombox. Default a bright blue. */
  accentColor?: number;
  /** Reference distance for the positional rolloff. Default 0.6. */
  refDistance?: number;
  /** Initial gain (0..1). Default 0.7. */
  gain?: number;
  /** Attempt to start playing on construction (still subject to autoplay
   *  policy). Default false — the UC4 mode-switcher controls play state. */
  autoplay?: boolean;
  /** Force a specific theme; default picks one at random per construction. */
  theme?: MusicTheme;
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
    size = 0.10,
    accentColor = 0x4ec5ff,     // bright cyan-blue per user direction
    refDistance = 0.6,
    gain = 0.7,
    autoplay = false,
    theme = pickRandomTheme(),
  } = opts;

  const group = new THREE.Group();
  group.name = 'live-spatial-audio';

  // ─── Boombox: blue line-wireframe rectangle + speaker rings ──────
  //
  // The body is a wireframe box outline plus 2 circular "speaker grilles"
  // built from line segments. No filled meshes — gives it a holographic
  // vibe that reads against passthrough.
  const lineMat = new THREE.LineBasicMaterial({ color: accentColor, transparent: true, opacity: 0.95 });

  const bodyGeo = new THREE.EdgesGeometry(new THREE.BoxGeometry(size * 2.4, size * 1.3, size * 0.9));
  const body = new THREE.LineSegments(bodyGeo, lineMat);
  group.add(body);

  // Two speaker rings on the front face. CircleGeometry → EdgesGeometry is
  // a circular fan; wrap with LineLoop to get just the perimeter circle.
  function makeSpeakerRing(): THREE.LineLoop {
    const segments = 32;
    const positions = new Float32Array((segments + 1) * 3);
    for (let i = 0; i <= segments; i++) {
      const a = (i / segments) * Math.PI * 2;
      positions[i * 3]     = Math.cos(a) * size * 0.42;
      positions[i * 3 + 1] = Math.sin(a) * size * 0.42;
      positions[i * 3 + 2] = 0;
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    return new THREE.LineLoop(g, lineMat);
  }
  const speakerL = makeSpeakerRing();
  speakerL.position.set(-size * 0.7, 0, size * 0.46);
  group.add(speakerL);
  const speakerR = makeSpeakerRing();
  speakerR.position.set( size * 0.7, 0, size * 0.46);
  group.add(speakerR);

  // Handle on top — bar of line segments.
  const handleGeo = new THREE.EdgesGeometry(
    new THREE.BoxGeometry(size * 1.0, size * 0.08, size * 0.12),
  );
  const handle = new THREE.LineSegments(handleGeo, lineMat);
  handle.position.set(0, size * 0.75, 0);
  group.add(handle);

  // ─── Play / Pause icon meshes (centered on the body) ─────────────
  //
  // Both are built; we swap visibility based on playback state.
  // Triangle "Play" — a filled mesh so it reads from any angle.
  const playGeo = new THREE.BufferGeometry();
  const ps = size * 0.18;
  playGeo.setAttribute('position', new THREE.BufferAttribute(new Float32Array([
    -ps * 0.6, -ps,         0,
    -ps * 0.6,  ps,         0,
     ps * 0.9,  0,          0,
  ]), 3));
  playGeo.computeVertexNormals();
  const iconMat = new THREE.MeshBasicMaterial({ color: accentColor, side: THREE.DoubleSide, transparent: true, opacity: 0.95 });
  const playIcon = new THREE.Mesh(playGeo, iconMat);
  playIcon.position.set(0, -size * 0.05, size * 0.46);
  group.add(playIcon);

  // Two-bar "Pause" — built from two thin filled rectangles.
  const pauseBarW = size * 0.10;
  const pauseBarH = size * 0.4;
  const pauseGroup = new THREE.Group();
  const barGeo = new THREE.PlaneGeometry(pauseBarW, pauseBarH);
  const barL = new THREE.Mesh(barGeo, iconMat);
  barL.position.set(-pauseBarW * 0.8, 0, 0);
  const barR = new THREE.Mesh(barGeo, iconMat);
  barR.position.set( pauseBarW * 0.8, 0, 0);
  pauseGroup.add(barL);
  pauseGroup.add(barR);
  pauseGroup.position.set(0, -size * 0.05, size * 0.46);
  group.add(pauseGroup);

  function setIconVisible(state: 'playing' | 'paused') {
    playIcon.visible = state === 'paused';     // show "Play" when paused (action affordance)
    pauseGroup.visible = state === 'playing';
  }
  setIconVisible('paused');

  // ─── Theme label below the boombox ────────────────────────────────
  const themeLabel = new Text();
  themeLabel.text = `${MUSIC_THEME_LABELS[theme]}`;
  themeLabel.fontSize = 0.020;
  themeLabel.color = accentColor;
  themeLabel.anchorX = 'center';
  themeLabel.anchorY = 'top';
  themeLabel.position.set(0, -size * 0.90, 0);
  themeLabel.sync();
  group.add(themeLabel);

  const themeSubLabel = new Text();
  themeSubLabel.text = 'NOW PLAYING';
  themeSubLabel.fontSize = 0.008;
  themeSubLabel.color = TEXT.muted;
  themeSubLabel.anchorX = 'center';
  themeSubLabel.anchorY = 'top';
  themeSubLabel.position.set(0, -size * 0.79, 0);
  themeSubLabel.sync();
  group.add(themeSubLabel);

  // ─── Particle plume — rises from the speakers while playing ──────
  //
  // 60 small floating spheres in a column above each speaker, with a slow
  // sinusoidal drift. Group is hidden while paused so they don't run the
  // animation off-screen.
  const PARTICLES_PER_SIDE = 18;
  const particleGroup = new THREE.Group();
  particleGroup.visible = false;
  const particleGeo = new THREE.SphereGeometry(size * 0.025, 6, 4);
  const particleMat = new THREE.MeshBasicMaterial({ color: accentColor, transparent: true, opacity: 0.7 });
  // Per-particle randomised offset + speed for the animation tick. Saved
  // outside the loop so onBeforeRender doesn't re-randomise per frame.
  const particles: Array<{ mesh: THREE.Mesh; seedX: number; seedY: number; period: number; }> = [];
  for (let side = 0; side < 2; side++) {
    for (let i = 0; i < PARTICLES_PER_SIDE; i++) {
      const m = new THREE.Mesh(particleGeo, particleMat);
      // Anchor the column above each speaker.
      const baseX = side === 0 ? -size * 0.7 : size * 0.7;
      const initialY = size * 0.7 + i * size * 0.07;
      m.position.set(baseX, initialY, size * 0.46);
      particleGroup.add(m);
      particles.push({
        mesh: m,
        seedX: Math.random() * Math.PI * 2,
        seedY: i,
        period: 1.5 + Math.random() * 1.0,
      });
    }
  }
  group.add(particleGroup);

  // Animate particles via onBeforeRender on the particleGroup. Each frame
  // each particle drifts up + side-to-side; when it passes the top of the
  // column it wraps to the bottom. Self-driven so it works in manifest
  // mode (no per-frame tick from the loader).
  let lastFrameMs = performance.now();
  particleGroup.onBeforeRender = () => {
    const now = performance.now();
    const dt = (now - lastFrameMs) / 1000;
    lastFrameMs = now;
    if (dt <= 0 || dt > 1) return;
    const verticalSpan = size * (0.7 + PARTICLES_PER_SIDE * 0.07);
    for (const p of particles) {
      // Rise rate scales with size so timing reads similarly on any boombox.
      p.mesh.position.y += dt * size * 0.6;
      if (p.mesh.position.y > size * 0.7 + verticalSpan) {
        p.mesh.position.y = size * 0.7;
      }
      // Gentle horizontal wobble — pseudo-random but deterministic.
      const base = p.mesh.position.x;
      const wobble = Math.sin(now / 1000 / p.period + p.seedX) * size * 0.04;
      p.mesh.position.x = Math.sign(base) * size * 0.7 + wobble;
      // Fade-in at the bottom, fade-out at the top.
      const t01 = (p.mesh.position.y - size * 0.7) / verticalSpan;
      p.mesh.scale.setScalar(Math.max(0.1, 1 - Math.abs(t01 - 0.4) * 1.6));
    }
  };

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
    void play();
  }

  if (listener) {
    positional = new THREE.PositionalAudio(listener);
    positional.setRefDistance(refDistance);
    positional.setRolloffFactor(1.0);
    positional.setDistanceModel('inverse');
    positional.setLoop(true);
    positional.setVolume(gain);
    positional.setBuffer(makeMusicLoopBuffer(listener.context, { theme }));
    body.add(positional);
  }

  async function play(): Promise<void> {
    if (!positional || !listener) return;
    const ctx = listener.context;
    if (ctx.state === 'suspended') {
      try { await ctx.resume(); } catch { /* defer below */ }
    }
    if (ctx.state !== 'running') {
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
    setIconVisible('playing');
    particleGroup.visible = true;
  }

  function pause() {
    if (!positional || !playing) return;
    positional.pause();
    playing = false;
    setIconVisible('paused');
    particleGroup.visible = false;
  }

  function setGain(g: number) {
    if (!positional) return;
    positional.setVolume(Math.max(0, Math.min(1, g)));
  }

  if (autoplay && positional) {
    play().catch(() => {});
  }

  return {
    group,
    play,
    pause,
    isPlaying: () => playing,
    setGain,
    tick: (_time: number) => { /* particles animate via onBeforeRender */ },
    dispose: () => {
      if (positional) {
        if (playing) positional.stop();
        body.remove(positional);
      }
      if (typeof window !== 'undefined') {
        window.removeEventListener('pointerdown', tryPlayDeferred);
        window.removeEventListener('keydown', tryPlayDeferred);
      }
      particleGroup.onBeforeRender = () => {};
      bodyGeo.dispose();
      handleGeo.dispose();
      lineMat.dispose();
      playGeo.dispose();
      barGeo.dispose();
      iconMat.dispose();
      particleGeo.dispose();
      particleMat.dispose();
      themeLabel.dispose();
      themeSubLabel.dispose();
    },
  };
}
