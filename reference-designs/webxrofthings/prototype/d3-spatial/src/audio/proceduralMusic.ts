/**
 * Procedural music loop generator — theme-distinct synthesis.
 *
 * Earlier iteration shared one chord-summing inner loop across every theme
 * and let only mix parameters vary. The four themes ended up sounding
 * nearly identical because the only audible difference was BPM and the
 * presence/absence of a single kick. This rewrite gives each theme its
 * own synthesis path:
 *
 *   - ambient    : sustained drone pad with slow detune-shimmer + 5th + octave.
 *                  No rhythm, no transients. ~32 s loop.
 *   - downtempo  : warm pad + kick on beats 1+3 + soft closed-hi-hat on 2+4.
 *                  ~85 BPM, 8-beat (one-bar) loop.
 *   - chillout   : pad + light hi-hat shuffle (8ths with swing) + brushed
 *                  snare ghost notes. ~76 BPM, 8-beat loop.
 *   - lofi       : classic boom-bap drum kit (K K K K? S _ S _) + short
 *                  chord stabs with vinyl noise floor + slow wow.
 *                  ~70 BPM, 16-beat loop.
 *
 * All buffers are mono and exact-length so PositionalAudio's seamless
 * loop works without a boundary click. Synthesis is deterministic (no
 * Math.random) so the same theme always produces the same buffer.
 */

export type MusicTheme = 'ambient' | 'downtempo' | 'chillout' | 'lofi';

export const MUSIC_THEME_LABELS: Record<MusicTheme, string> = {
  ambient:   'Ambient',
  downtempo: 'Downtempo',
  chillout:  'Chillout',
  lofi:      'Lo-Fi Hip Hop',
};

/** Pick a random theme each load — gives the boombox a tiny bit of surprise. */
export function pickRandomTheme(): MusicTheme {
  const themes: MusicTheme[] = ['ambient', 'downtempo', 'chillout', 'lofi'];
  return themes[Math.floor(Math.random() * themes.length)]!;
}

export interface MusicLoopOpts {
  theme?: MusicTheme;
  /** Master amplitude override (0..1). Defaults to the theme's master. */
  gain?: number;
}

// ── deterministic helpers ────────────────────────────────────────────

/** LCG-based pseudo-random in [0, 1) keyed by the sample index. */
function lcg(i: number): number {
  return ((i * 1664525 + 1013904223) >>> 0) / 0xffffffff;
}

/** Soft tanh saturation. */
function sat(x: number, drive: number): number {
  return Math.tanh(x * drive);
}

/** Kick: 60 → 40 Hz exponential pitch sweep, short exp-decay envelope. */
function kick(tSec: number): number {
  if (tSec < 0 || tSec > 0.45) return 0;
  const env = Math.exp(-tSec * 14);
  const sweep = 60 * Math.exp(-tSec * 8) + 35;
  return Math.sin(2 * Math.PI * sweep * tSec) * env;
}

/** Snare: noise burst + 200 Hz body, fast exp decay. */
function snare(tSec: number, i: number): number {
  if (tSec < 0 || tSec > 0.30) return 0;
  const env = Math.exp(-tSec * 22);
  const noise = (lcg(i * 7 + 11) - 0.5) * 2;
  const body = Math.sin(2 * Math.PI * 200 * tSec) * 0.4;
  return (noise + body) * env;
}

/** Closed hi-hat: very short high-passed noise burst. */
function hatClosed(tSec: number, i: number): number {
  if (tSec < 0 || tSec > 0.08) return 0;
  const env = Math.exp(-tSec * 70);
  const n1 = lcg(i * 13 + 41) - 0.5;
  const n2 = lcg(i * 17 + 97) - 0.5;
  return (n1 - n2) * env;   // crude high-pass
}

/** Cosine-shaped attack/release window over `lengthSec` starting at 0. */
function adsr(tSec: number, attack: number, release: number, lengthSec: number): number {
  if (tSec < 0 || tSec > lengthSec) return 0;
  if (tSec < attack) return tSec / attack;
  if (tSec > lengthSec - release) return (lengthSec - tSec) / release;
  return 1;
}

// ── per-theme synthesis ──────────────────────────────────────────────

/**
 * Ambient drone: three detuned sine partials per chord-tone with very slow
 * tremolo, no transients. Two chords blend over a 32-second cycle.
 */
function synthAmbient(ch: Float32Array, sr: number): void {
  const chords: number[][] = [
    [196.00, 261.63, 329.63, 392.00],   // G2 C4 E4 G4 — open Csus2-ish
    [220.00, 293.66, 349.23, 440.00],   // A2 D4 F4 A4 — drift to Dm
  ];
  const n = ch.length;
  for (let i = 0; i < n; i++) {
    const t = i / sr;
    // Slow crossfade between the two chords (full cycle = the loop length).
    const xfade = 0.5 - 0.5 * Math.cos(2 * Math.PI * (i / n));
    let s = 0;
    for (let ci = 0; ci < 2; ci++) {
      const w = ci === 0 ? (1 - xfade) : xfade;
      for (const f of chords[ci]!) {
        // Three partials per note, slightly detuned to give the pad
        // movement. The slow tremolo (~0.07 Hz) is the only "rhythm."
        const trem = 0.85 + 0.15 * Math.sin(2 * Math.PI * 0.07 * t + f * 0.001);
        s += Math.sin(2 * Math.PI * f * t) * 0.10 * w * trem;
        s += Math.sin(2 * Math.PI * (f * 1.003) * t) * 0.08 * w * trem;
        s += Math.sin(2 * Math.PI * (f * 0.5) * t) * 0.06 * w * trem;
      }
    }
    ch[i] = sat(s, 0.7) * 0.42;
  }
}

/**
 * Downtempo: pad + kick on beats 1, 3 + soft closed hat on 2, 4. 85 BPM.
 * Chord progression I-V-vi-IV in C major, one bar per loop.
 */
function synthDowntempo(ch: Float32Array, sr: number, bpm: number): void {
  const progression: number[][] = [
    [261.63, 329.63, 392.00],    // C
    [392.00, 493.88, 587.33],    // G
    [220.00, 261.63, 329.63],    // Am (lower voicing)
    [349.23, 440.00, 523.25],    // F
  ];
  const n = ch.length;
  const beats = 4 * progression.length;     // one bar per chord (4 beats each)
  const samplesPerBeat = sr * 60 / bpm;
  const samplesPerChord = sr * (60 / bpm) * 4;

  for (let i = 0; i < n; i++) {
    const t = i / sr;
    const chordIdx = Math.min(progression.length - 1, Math.floor(i / samplesPerChord));
    const chord = progression[chordIdx]!;
    const chordPhase = (i - chordIdx * samplesPerChord) / samplesPerChord;
    const padEnv = adsr(chordPhase, 0.12, 0.18, 1.0);

    // Pad: triad + octave-up partial, gentle.
    let pad = 0;
    for (const f of chord) {
      pad += Math.sin(2 * Math.PI * f * t) * 0.16;
      pad += Math.sin(2 * Math.PI * f * 2 * t) * 0.05;
    }
    pad *= padEnv * 0.9;

    // Drums: kick on beats 1, 3 ; hat on beats 2, 4 (within each bar).
    const beatPos = i / samplesPerBeat;
    const beatIdx = Math.floor(beatPos) % beats;
    const beatPhaseSec = (i % samplesPerBeat) / sr;
    let drums = 0;
    if (beatIdx % 4 === 0 || beatIdx % 4 === 2) {
      drums += kick(beatPhaseSec) * 0.55;
    }
    if (beatIdx % 4 === 1 || beatIdx % 4 === 3) {
      drums += hatClosed(beatPhaseSec, i) * 0.20;
    }

    ch[i] = sat(pad + drums, 1.0) * 0.40;
  }
}

/**
 * Chillout: pad + 8th-note hat shuffle (swing) + ghost snare on the &-of-2.
 * 76 BPM, ii-V-I-IV progression. No kick — feels like a beach-bar loop.
 */
function synthChillout(ch: Float32Array, sr: number, bpm: number): void {
  const progression: number[][] = [
    [293.66, 349.23, 440.00],    // Dm
    [392.00, 493.88, 587.33],    // G
    [261.63, 329.63, 392.00],    // C
    [349.23, 440.00, 523.25],    // F
  ];
  const n = ch.length;
  const samplesPerBeat = sr * 60 / bpm;
  const samplesPerChord = samplesPerBeat * 4;
  const samplesPer8th = samplesPerBeat / 2;
  // Swing: the off-beat 8th sits slightly late.
  const swingDelay = samplesPer8th * 0.14;

  for (let i = 0; i < n; i++) {
    const t = i / sr;
    const chordIdx = Math.min(progression.length - 1, Math.floor(i / samplesPerChord));
    const chord = progression[chordIdx]!;
    const chordPhase = (i - chordIdx * samplesPerChord) / samplesPerChord;
    const padEnv = adsr(chordPhase, 0.10, 0.22, 1.0);

    let pad = 0;
    for (const f of chord) {
      pad += Math.sin(2 * Math.PI * f * t) * 0.14;
      pad += Math.sin(2 * Math.PI * f * 1.5 * t) * 0.04;   // 5th partial
    }
    pad *= padEnv * 0.85;

    // Hat on every 8th, off-beats delayed for swing.
    const eighthIdx = Math.floor(i / samplesPer8th);
    const eighthStart = eighthIdx * samplesPer8th + ((eighthIdx & 1) ? swingDelay : 0);
    const hatPhaseSec = (i - eighthStart) / sr;
    let drums = hatClosed(hatPhaseSec, i) * ((eighthIdx & 1) ? 0.12 : 0.18);

    // Ghost snare on the "&" of beat 2 of each bar — very quiet.
    const bar = Math.floor(i / (samplesPerBeat * 4));
    const ghostStart = bar * samplesPerBeat * 4 + samplesPerBeat * 2.5;
    drums += snare((i - ghostStart) / sr, i) * 0.08;

    ch[i] = sat(pad + drums, 1.0) * 0.40;
  }
}

/**
 * Lo-Fi Hip Hop: full kit (K on 1+3, S on 2+4, hat on every 8th) + short
 * chord stabs + vinyl hiss + slow amplitude wow. 70 BPM, two bars per loop.
 */
function synthLofi(ch: Float32Array, sr: number, bpm: number): void {
  const progression: number[][] = [
    [261.63, 311.13, 392.00, 466.16],   // Cm7 (C E♭ G B♭)
    [349.23, 415.30, 523.25, 622.25],   // Fm7
    [311.13, 392.00, 466.16, 587.33],   // E♭maj7-ish
    [233.08, 293.66, 349.23, 440.00],   // B♭ A
  ];
  const n = ch.length;
  const samplesPerBeat = sr * 60 / bpm;
  const samplesPer8th = samplesPerBeat / 2;
  const samplesPerChord = samplesPerBeat * 4;   // one bar per chord
  // Stabs trigger on beats 1 and 3 of each bar — classic boom-bap chord chop.
  const stabLengthSec = 0.6;

  for (let i = 0; i < n; i++) {
    const t = i / sr;
    const chordIdx = Math.min(progression.length - 1, Math.floor(i / samplesPerChord));
    const chord = progression[chordIdx]!;

    // Stab envelope: short attack, gentle decay, fully off between stabs.
    const beatInBar = (i % (samplesPerBeat * 4)) / samplesPerBeat;
    let stabPhaseSec = -1;
    if (beatInBar >= 0 && beatInBar < 1) stabPhaseSec = beatInBar * 60 / bpm;
    else if (beatInBar >= 2 && beatInBar < 3) stabPhaseSec = (beatInBar - 2) * 60 / bpm;
    let stab = 0;
    if (stabPhaseSec >= 0 && stabPhaseSec < stabLengthSec) {
      const env = Math.exp(-stabPhaseSec * 4) * (stabPhaseSec < 0.02 ? stabPhaseSec / 0.02 : 1);
      for (const f of chord) {
        stab += Math.sin(2 * Math.PI * f * t) * 0.18;
      }
      stab *= env;
    }

    // Drums.
    const beatPos = i / samplesPerBeat;
    const beatIdx = Math.floor(beatPos);
    const beatPhaseSec = (i % samplesPerBeat) / sr;
    const eighthIdx = Math.floor(i / samplesPer8th);
    const eighthPhaseSec = (i % samplesPer8th) / sr;

    let drums = 0;
    if (beatIdx % 4 === 0 || beatIdx % 4 === 2) {
      drums += kick(beatPhaseSec) * 0.70;
    }
    if (beatIdx % 4 === 1 || beatIdx % 4 === 3) {
      drums += snare(beatPhaseSec, i) * 0.40;
    }
    // Hat on every 8th, quieter on the downbeats so kick + snare punch.
    drums += hatClosed(eighthPhaseSec, i) * ((eighthIdx & 1) ? 0.18 : 0.10);

    // Vinyl hiss — continuous low-amplitude noise floor.
    const hiss = (lcg(i) - 0.5) * 0.07;

    // Slow wow (~0.5 Hz) on the whole signal — the tape-flutter feel.
    const wow = 0.94 + 0.06 * Math.sin(2 * Math.PI * 0.5 * t);

    ch[i] = sat((stab + drums + hiss) * wow, 1.3) * 0.40;
  }
}

interface ThemeDef {
  loopSeconds: number;
  bpm: number;
  render(ch: Float32Array, sr: number, bpm: number): void;
}

const THEMES: Record<MusicTheme, ThemeDef> = {
  ambient:   { loopSeconds: 32, bpm: 60, render: (ch, sr) => synthAmbient(ch, sr) },
  downtempo: { loopSeconds: 8 * 60 / 85, bpm: 85, render: synthDowntempo },
  chillout:  { loopSeconds: 8 * 60 / 76, bpm: 76, render: synthChillout },
  // Two bars (8 beats) at 70 BPM ≈ 6.86 s — long enough for the drum
  // pattern to feel like a loop, short enough to keep the buffer small.
  lofi:      { loopSeconds: 8 * 60 / 70, bpm: 70, render: synthLofi },
};

/**
 * Build a mono AudioBuffer carrying the looping music for `theme`. The
 * buffer length is the exact loopSeconds so PositionalAudio.setLoop(true)
 * produces a seamless loop without a boundary click.
 */
export function makeMusicLoopBuffer(
  ctx: BaseAudioContext,
  opts: MusicLoopOpts = {},
): AudioBuffer {
  const theme = opts.theme ?? 'downtempo';
  const def = THEMES[theme];
  const sr = ctx.sampleRate;
  const n = Math.floor(sr * def.loopSeconds);
  const buf = ctx.createBuffer(1, n, sr);
  const ch = buf.getChannelData(0);
  def.render(ch, sr, def.bpm);

  // Final master-gain pass — applied here rather than inside each synth
  // function so an explicit `opts.gain` override behaves predictably.
  const masterGain = opts.gain ?? 1.0;
  if (masterGain !== 1.0) {
    for (let i = 0; i < n; i++) ch[i] = ch[i]! * masterGain;
  }
  return buf;
}
