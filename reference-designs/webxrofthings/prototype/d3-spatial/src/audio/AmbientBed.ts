import * as THREE from 'three';

/**
 * AmbientBed — opt-in, per-dataspace ambisonic ambient audio.
 *
 * Design intent (see XR_UX-proposal1.md §8):
 *   - A dataspace manifest MAY publish `ambisonicBedUrl` (ambix-format .ogg).
 *   - The user must opt in via the HUD; auto-play is forbidden.
 *   - Decoding runs through Omnitone (FOARenderer / HOARenderer) so the bed
 *     rotates with listener head pose.
 *
 * This module is a skeleton: it establishes the API surface and wires the
 * head-pose update loop. Actual Omnitone import is lazy so the prototype
 * bundle does not pay for it until a bed is requested.
 */
export interface AmbientBedOptions {
  url: string;
  order?: 1 | 2 | 3;
  gain?: number;
}

export class AmbientBed {
  private renderer: any = null;
  private source: AudioBufferSourceNode | null = null;
  private gainNode: GainNode | null = null;
  private listenerObj: THREE.Object3D;

  constructor(private ctx: AudioContext, listener: THREE.Object3D) {
    this.listenerObj = listener;
  }

  async load(opts: AmbientBedOptions): Promise<void> {
    const { url, order = 1, gain = 0.5 } = opts;
    const buf = await (await fetch(url)).arrayBuffer();
    const decoded = await this.ctx.decodeAudioData(buf);
    await this.loadFromBuffer(decoded, { order, gain });
  }

  async loadFromBuffer(
    buffer: AudioBuffer,
    opts: { order?: 1 | 2 | 3; gain?: number } = {},
  ): Promise<void> {
    const { order = 1, gain = 0.5 } = opts;
    const Omnitone = await import('omnitone/build/omnitone.esm.js').then(m => (m as any).default ?? m);
    this.renderer = order === 1
      ? Omnitone.createFOARenderer(this.ctx, { ambisonicOrder: 1 })
      : Omnitone.createHOARenderer(this.ctx, { ambisonicOrder: order });
    await this.renderer.initialize();

    this.source = this.ctx.createBufferSource();
    this.source.buffer = buffer;
    this.source.loop = true;

    this.gainNode = this.ctx.createGain();
    this.gainNode.gain.value = gain;

    this.source.connect(this.renderer.input);
    this.renderer.output.connect(this.gainNode);
    this.gainNode.connect(this.ctx.destination);
  }

  start() { this.source?.start(0); }

  stop() {
    try { this.source?.stop(); } catch {}
    this.source = null;
  }

  /** Call from the render loop so the bed rotates with head pose. */
  update() {
    if (!this.renderer) return;
    const m = this.listenerObj.matrixWorld;
    this.renderer.setRotationMatrix4(m.elements);
  }

  setGain(v: number) { if (this.gainNode) this.gainNode.gain.value = v; }
}
