/**
 * Procedural music loop generator.
 *
 * Same shape as `makeAmbientFoaBuffer` in `proceduralBed.ts` but tuned for
 * the UC4 "boombox" — a single-channel (mono) AudioBuffer suitable for
 * THREE.PositionalAudio. Building the loop in code keeps us off the asset
 * pipeline: no MP3 to ship, no license to track, no bundle-size cost
 * beyond a few hundred lines of code.
 *
 * The progression is I-V-vi-IV in C major (260/195/220/175 Hz fundamentals),
 * one of the most-used pop progressions because it's pleasantly inert.
 * Each chord is a major/minor triad of sines layered with an octave-up
 * partial for harmonic interest, gently saturated to glue the sum.
 *
 * Loop length defaults to 8 seconds at 4 chords × 2 s. The buffer is
 * sample-accurate so it loops cleanly via `pa.setLoop(true)`.
 */

interface Chord {
  /** Triad fundamental frequencies in Hz, lowest first. */
  triad: [number, number, number];
}

/** I-V-vi-IV in C major. */
const PROGRESSION: Chord[] = [
  { triad: [261.63, 329.63, 392.00] },  // I  · C major  (C E G)
  { triad: [392.00, 493.88, 587.33] },  // V  · G major  (G B D)
  { triad: [440.00, 523.25, 659.25] },  // vi · A minor  (A C E)
  { triad: [349.23, 440.00, 523.25] },  // IV · F major  (F A C)
];

export interface MusicLoopOpts {
  /** Total loop length in seconds. Default 8 (2 s per chord). */
  seconds?: number;
  /** Master amplitude (0..1). Default 0.45 — boombox is a foreground sound. */
  gain?: number;
  /** Chord progression override. Default is the I-V-vi-IV documented above. */
  progression?: Chord[];
}

/**
 * Build a mono AudioBuffer carrying the looping music. The returned buffer
 * is exact in length so `PositionalAudio.setLoop(true)` produces a
 * seamless loop with no click at the boundary.
 */
export function makeMusicLoopBuffer(
  ctx: BaseAudioContext,
  opts: MusicLoopOpts = {},
): AudioBuffer {
  const seconds = opts.seconds ?? 8;
  const gain = opts.gain ?? 0.45;
  const chords = opts.progression ?? PROGRESSION;

  const sr = ctx.sampleRate;
  const n = Math.floor(sr * seconds);
  const buf = ctx.createBuffer(1, n, sr);
  const ch = buf.getChannelData(0);

  const chordLenSamples = n / chords.length;

  for (let i = 0; i < n; i++) {
    const t = i / sr;
    const ci = Math.min(chords.length - 1, Math.floor(i / chordLenSamples));
    const chord = chords[ci]!;
    // Position within this chord, 0..1 — drives the cross-fade envelope so
    // adjacent chords don't click into each other at the boundary.
    const chordPhase = (i - ci * chordLenSamples) / chordLenSamples;
    const envIn  = Math.min(chordPhase * 6, 1);            // fade-in over first ~1/6
    const envOut = Math.min((1 - chordPhase) * 6, 1);      // fade-out over last ~1/6
    const env = envIn * envOut * 0.65 + 0.35;              // stays above silence

    let s = 0;
    for (const f of chord.triad) {
      // Fundamental + octave partial. The octave is at 1/3 the amplitude
      // to add brightness without making it harsh.
      s += Math.sin(2 * Math.PI * f * t) * 0.20;
      s += Math.sin(2 * Math.PI * f * 2 * t) * 0.07;
    }
    s *= env;

    // Soft tanh saturation to glue partials and contain the dynamic range.
    ch[i] = Math.tanh(s * 0.9) * gain;
  }

  return buf;
}
