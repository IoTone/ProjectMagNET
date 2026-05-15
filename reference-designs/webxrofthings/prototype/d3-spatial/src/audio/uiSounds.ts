/**
 * uiSounds — tiny procedural UI feedback blips.
 *
 * Two sounds, synthesised on the shared Web Audio context (no assets,
 * same philosophy as proceduralMusic):
 *
 *   - playFocus()  a soft sine "tick-up" when a control gains hover/focus
 *   - playClick()  a snappy "cherry-MX"-ish click on select
 *
 * Both reuse the scene's AudioListener context exposed by main.ts via
 * `window.__demo.audioListener`. If it's missing (node tests, pre-init)
 * or the context is still suspended (autoplay policy, before the first
 * gesture) the calls are silent no-ops — never throw, never block UI.
 * UI feedback is intentionally NON-spatial (connected straight to
 * destination): it should read the same regardless of head pose.
 */

function getCtx(): AudioContext | null {
  if (typeof window === 'undefined') return null;
  const l = (window as unknown as { __demo?: { audioListener?: { context?: AudioContext } } }).__demo;
  const ctx = l?.audioListener?.context ?? null;
  // Only play once the context is actually running — calling start() on a
  // suspended context queues sounds that fire in a confusing burst when
  // it later resumes.
  return ctx && ctx.state === 'running' ? ctx : null;
}

/** Soft rising sine, ~90 ms. Plays on hover/focus of a control. */
export function playFocus(): void {
  const c = getCtx();
  if (!c) return;
  const t = c.currentTime;
  const osc = c.createOscillator();
  const g = c.createGain();
  osc.type = 'sine';
  osc.frequency.setValueAtTime(660, t);
  osc.frequency.exponentialRampToValueAtTime(880, t + 0.05);
  g.gain.setValueAtTime(0.0001, t);
  g.gain.exponentialRampToValueAtTime(0.10, t + 0.008);   // fast attack
  g.gain.exponentialRampToValueAtTime(0.0001, t + 0.09);  // short decay
  osc.connect(g).connect(c.destination);
  osc.start(t);
  osc.stop(t + 0.10);
}

/**
 * "Cherry-MX"-style click: a short band-passed noise burst (the clack)
 * layered with a quick downward-pitched triangle tick (the snap). ~55 ms.
 */
export function playClick(): void {
  const c = getCtx();
  if (!c) return;
  const t = c.currentTime;

  // Clack — exponentially-decaying white noise through a bandpass.
  const dur = 0.05;
  const n = Math.max(1, Math.ceil(c.sampleRate * dur));
  const buf = c.createBuffer(1, n, c.sampleRate);
  const d = buf.getChannelData(0);
  for (let i = 0; i < n; i++) {
    const env = (1 - i / n) ** 2;          // sharp front, fast tail
    d[i] = (Math.random() * 2 - 1) * env;
  }
  const noise = c.createBufferSource();
  noise.buffer = buf;
  const bp = c.createBiquadFilter();
  bp.type = 'bandpass';
  bp.frequency.value = 2300;
  bp.Q.value = 1.1;
  const ng = c.createGain();
  ng.gain.value = 0.20;
  noise.connect(bp).connect(ng).connect(c.destination);

  // Snap — short pitched tick for the mechanical "cherry" character.
  const osc = c.createOscillator();
  const og = c.createGain();
  osc.type = 'triangle';
  osc.frequency.setValueAtTime(1500, t);
  osc.frequency.exponentialRampToValueAtTime(620, t + 0.03);
  og.gain.setValueAtTime(0.16, t);
  og.gain.exponentialRampToValueAtTime(0.0001, t + 0.045);
  osc.connect(og).connect(c.destination);

  noise.start(t);
  osc.start(t);
  osc.stop(t + 0.05);
}
