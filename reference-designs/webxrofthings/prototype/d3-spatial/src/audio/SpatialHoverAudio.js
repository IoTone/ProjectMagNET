import * as THREE from 'three';
export class SpatialHoverAudio {
    defaultCamera;
    listener = new THREE.AudioListener();
    ctx = null;
    buffer = null;
    perNode = new Map();
    ready = false;
    constructor(defaultCamera) {
        this.defaultCamera = defaultCamera;
        defaultCamera.add(this.listener);
    }
    reparentTo(obj) {
        if (this.listener.parent)
            this.listener.parent.remove(this.listener);
        obj.add(this.listener);
    }
    async init() {
        if (this.ready)
            return;
        this.ctx = this.listener.context;
        this.buffer = tick(this.ctx, 660, 0.06);
        this.ready = true;
    }
    attach(id, object) {
        if (this.perNode.has(id))
            return;
        const pa = new THREE.PositionalAudio(this.listener);
        pa.setRefDistance(0.2);
        pa.setRolloffFactor(1.2);
        pa.setDistanceModel('inverse');
        object.add(pa);
        this.perNode.set(id, pa);
    }
    play(id) {
        if (!this.ready || !this.buffer)
            return;
        const pa = this.perNode.get(id);
        if (!pa)
            return;
        if (pa.isPlaying)
            pa.stop();
        pa.setBuffer(this.buffer);
        pa.play();
    }
}
function tick(ctx, freq, dur) {
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
