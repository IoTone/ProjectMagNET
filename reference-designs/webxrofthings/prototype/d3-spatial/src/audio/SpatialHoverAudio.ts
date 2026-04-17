import * as THREE from 'three';

export class SpatialHoverAudio {
  readonly listener = new THREE.AudioListener();
  private ctx: AudioContext | null = null;
  private buffer: AudioBuffer | null = null;
  private perNode = new Map<string, THREE.PositionalAudio>();
  private ready = false;

  constructor(private defaultCamera: THREE.Camera) {
    defaultCamera.add(this.listener);
  }

  reparentTo(obj: THREE.Object3D) {
    if (this.listener.parent) this.listener.parent.remove(this.listener);
    obj.add(this.listener);
  }

  async init() {
    if (this.ready) return;
    this.ctx = this.listener.context as AudioContext;
    this.buffer = tick(this.ctx, 660, 0.06);
    this.ready = true;
  }

  attach(id: string, object: THREE.Object3D) {
    if (this.perNode.has(id)) return;
    const pa = new THREE.PositionalAudio(this.listener);
    pa.setRefDistance(0.2);
    pa.setRolloffFactor(1.2);
    pa.setDistanceModel('inverse');
    object.add(pa);
    this.perNode.set(id, pa);
  }

  play(id: string) {
    if (!this.ready || !this.buffer) return;
    const pa = this.perNode.get(id);
    if (!pa) return;
    if (pa.isPlaying) pa.stop();
    pa.setBuffer(this.buffer);
    pa.play();
  }
}

function tick(ctx: AudioContext, freq: number, dur: number): AudioBuffer {
  const sr = ctx.sampleRate;
  const n = Math.floor(sr * dur);
  const buf = ctx.createBuffer(1, n, sr);
  const ch = buf.getChannelData(0);
  for (let i = 0; i < n; i++) {
    const t = i / sr;
    const env = Math.exp(-t * 40);
    ch[i] = Math.sin(2 * Math.PI * freq * t) * env * 0.4;
  }
  return buf;
}
