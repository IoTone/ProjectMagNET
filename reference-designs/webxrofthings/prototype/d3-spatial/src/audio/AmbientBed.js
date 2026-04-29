export class AmbientBed {
    ctx;
    renderer = null;
    source = null;
    gainNode = null;
    listenerObj;
    constructor(ctx, listener) {
        this.ctx = ctx;
        this.listenerObj = listener;
    }
    async load(opts) {
        const { url, order = 1, gain = 0.5 } = opts;
        const buf = await (await fetch(url)).arrayBuffer();
        const decoded = await this.ctx.decodeAudioData(buf);
        await this.loadFromBuffer(decoded, { order, gain });
    }
    async loadFromBuffer(buffer, opts = {}) {
        const { order = 1, gain = 0.5 } = opts;
        const Omnitone = await import('omnitone/build/omnitone.esm.js').then(m => m.default ?? m);
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
        try {
            this.source?.stop();
        }
        catch { }
        this.source = null;
    }
    /** Call from the render loop so the bed rotates with head pose. */
    update() {
        if (!this.renderer)
            return;
        const m = this.listenerObj.matrixWorld;
        this.renderer.setRotationMatrix4(m.elements);
    }
    setGain(v) { if (this.gainNode)
        this.gainNode.gain.value = v; }
}
