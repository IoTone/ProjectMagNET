/** Build a 4-channel FOA (W, X, Y, Z) ambient drone buffer. */
export function makeAmbientFoaBuffer(ctx: AudioContext, seconds = 4): AudioBuffer {
  const sr = ctx.sampleRate;
  const n = Math.floor(sr * seconds);
  const buf = ctx.createBuffer(4, n, sr);

  const w = buf.getChannelData(0);
  const x = buf.getChannelData(1);
  const y = buf.getChannelData(2);
  const z = buf.getChannelData(3);

  const drones = [
    { f: 110, a: 0.22, pan: { x: 0.8, y: 0.2, z: 0 } },
    { f: 165, a: 0.18, pan: { x: -0.6, y: 0.4, z: 0.1 } },
    { f: 220, a: 0.12, pan: { x: 0.1, y: -0.3, z: 0.2 } },
  ];

  for (let i = 0; i < n; i++) {
    const t = i / sr;
    let s = 0;
    for (const d of drones) s += Math.sin(2 * Math.PI * d.f * t) * d.a;
    const env = 0.5 + 0.5 * Math.sin(2 * Math.PI * (t / seconds));
    s *= env;
    w[i] = s * 0.35;
    x[i] = s * 0.4 * drones[0]!.pan.x;
    y[i] = s * 0.4 * drones[1]!.pan.y;
    z[i] = s * 0.4 * drones[2]!.pan.z;
  }
  return buf;
}
