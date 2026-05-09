/**
 * ambientController — owns the FOA ambient bed lifecycle (start / stop /
 * toggle / state) and the small "♪ ambient: …" HUD label that mirrors it.
 * Lifted out of main.ts so the orchestrator just constructs and listens.
 *
 * The HUD label is a `troika-three-text` Text instance — caller adds it to
 * its preferred UI anchor and positions it. Each state transition re-syncs
 * label and color in place.
 */

import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { AmbientBed } from './AmbientBed';
import { makeAmbientFoaBuffer } from './proceduralBed';
import { SpatialHoverAudio } from './SpatialHoverAudio';
import { TEXT } from '../ui/palette';

export type AmbientState = 'off' | 'loading' | 'on' | 'error';

export interface AmbientController {
  /** Begin loading + playback. Idempotent: no-op when already loading/on. */
  start(): Promise<void>;
  /** Stop and free the active bed. No-op when off. */
  stop(): void;
  /** Convenience for the `toggle-ambient` HUD action. */
  toggle(): Promise<void>;
  getState(): AmbientState;
  /** HUD label that mirrors the controller state. Add to your UI anchor and position. */
  hud: Text;
}

const BADGES: Record<AmbientState, { text: string; color: number }> = {
  off:     { text: '♪ ambient: off',                            color: TEXT.dim },
  loading: { text: '♪ ambient: loading…',                       color: TEXT.warn },
  on:      { text: '♪ ambient: ON · 4ch FOA · HRTF rotating',   color: TEXT.accent },
  error:   { text: '♪ ambient: error',                          color: TEXT.error },
};

export interface AmbientControllerOptions {
  audio: SpatialHoverAudio;
  camera: THREE.Camera;
  /** FOA bed length in seconds (default 4). */
  bufferSeconds?: number;
  /** Linear gain on the bed (default 0.35). */
  gain?: number;
}

export function createAmbientController(opts: AmbientControllerOptions): AmbientController {
  const { audio, camera, bufferSeconds = 4, gain = 0.35 } = opts;

  let bed: AmbientBed | null = null;
  let state: AmbientState = 'off';

  const hud = new Text();
  hud.text = BADGES.off.text;
  hud.fontSize = 0.014;
  hud.color = TEXT.muted;
  hud.anchorX = 'left';
  hud.anchorY = 'bottom';
  hud.sync();

  function updateHud() {
    const b = BADGES[state];
    hud.text = b.text;
    hud.color = b.color;
    hud.sync();
  }

  async function start() {
    if (state !== 'off' && state !== 'error') return;
    state = 'loading';
    updateHud();
    try {
      await audio.init();
      const ctx = audio.listener.context as AudioContext;
      if (ctx.state === 'suspended') await ctx.resume();
      const buf = makeAmbientFoaBuffer(ctx, bufferSeconds);
      bed = new AmbientBed(ctx, camera);
      await bed.loadFromBuffer(buf, { order: 1, gain });
      bed.start();
      state = 'on';
    } catch (e) {
      console.error('[ambient] failed:', e);
      state = 'error';
    }
    updateHud();
  }

  function stop() {
    if (!bed) return;
    bed.stop();
    bed = null;
    state = 'off';
    updateHud();
  }

  async function toggle() {
    if (state === 'on') stop();
    else await start();
  }

  return { start, stop, toggle, getState: () => state, hud };
}
