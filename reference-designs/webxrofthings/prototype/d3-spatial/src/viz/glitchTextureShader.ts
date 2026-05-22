/*
 * glitchTextureShader — per-mesh "VHS / CRT glitch" effect for a textured
 * plane. Replaces what would otherwise be a postprocessing EffectComposer
 * full-screen pass with a self-contained ShaderMaterial; the rest of the
 * scene renders cleanly, and the effect survives WebXR's multi-view +
 * foveated-rendering path that postprocessing chains are known to break.
 *
 * Visual recipe (matches the pmndrs/postprocessing GlitchEffect look):
 *   - Slow base "intensity" oscillation with occasional burst peaks
 *     (high-amplitude windows of ~50–200 ms every few seconds).
 *   - Horizontal stripe displacement: divide UV.y into ~40 bands; per
 *     band, a stepped random offset shifts UV.x by up to a few percent.
 *     Threshold + step keeps most bands stable so the artifact reads as
 *     occasional torn slabs rather than a continuous wave.
 *   - Chromatic aberration: sample R / B with small UV offsets opposite
 *     each other, hold G centred. Amplitude scales with intensity.
 *   - Rare per-band scrambling: a band's UV.x gets re-mapped to a
 *     totally different region of the texture. Tiny probability per
 *     band per frame so it reads as a brief "frame tear."
 *
 * The shader uses one uTexture + one uTime uniform. Caller updates uTime
 * in an onBeforeRender hook.
 */

import * as THREE from 'three';

const VERT = /* glsl */`
  varying vec2 vUv;
  void main() {
    vUv = uv;
    gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
  }
`;

const FRAG = /* glsl */`
  uniform sampler2D uTexture;
  uniform float     uTime;
  varying vec2      vUv;

  /* Trivial 2D hash — adequate for visual noise, no need for cryptographic
   * quality. Avoid GLES2 quirks (no bitops) by using the fract-sin trick. */
  float rand(vec2 co) {
    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
  }

  void main() {
    vec2 uv = vUv;

    /* Base intensity + bursts. The burst envelope is a smoothstep on a
     * slow sine so the ramp-up and ramp-down feel organic instead of a
     * hard on/off. Intensity in [0.3, 1.8] roughly. */
    float burst     = smoothstep(0.92, 1.0, sin(uTime * 0.7) * 0.5 + 0.5);
    float intensity = 0.3 + burst * 1.5;

    /* Horizontal stripe displacement. floor(uv.y * 40) gives 40 bands;
     * each band picks a random offset per timestep. step(0.85, …) keeps
     * 85% of bands at zero displacement so most of the image is stable. */
    float stripe      = floor(uv.y * 40.0);
    float stripeRand  = rand(vec2(stripe, floor(uTime * 8.0)));
    float displaceAmt = (stripeRand - 0.5) * 0.04 * intensity
                      * step(0.85, stripeRand);
    uv.x += displaceAmt;

    /* Chromatic aberration. R offset right, B offset left; amplitude
     * scales with intensity so quiet frames have a hint of CA, burst
     * frames go full neon. */
    float ca = 0.003 + burst * 0.008;
    float r = texture2D(uTexture, uv + vec2( ca, 0.0)).r;
    float g = texture2D(uTexture, uv                  ).g;
    float b = texture2D(uTexture, uv + vec2(-ca, 0.0)).b;

    /* Rare per-band frame tear: pick a random region of the texture and
     * sample from there for this band. ~3% chance per band per timestep. */
    if (rand(vec2(stripe, floor(uTime * 12.0))) > 0.97) {
      float scrambledX = fract(uv.x + rand(vec2(stripe, 0.0)) * 0.5);
      vec3 tear = texture2D(uTexture, vec2(scrambledX, uv.y)).rgb;
      r = tear.r; g = tear.g; b = tear.b;
    }

    gl_FragColor = vec4(r, g, b, 1.0);
  }
`;

export interface GlitchMaterial extends THREE.ShaderMaterial {
  /** Caller updates this in an onBeforeRender hook each frame.
   *  Wrapped here so callers don't have to type out the uniforms path. */
  setTime(seconds: number): void;
}

export function createGlitchMaterial(texture: THREE.Texture): GlitchMaterial {
  const mat = new THREE.ShaderMaterial({
    uniforms: {
      uTexture: { value: texture },
      uTime:    { value: 0.0 },
    },
    vertexShader:   VERT,
    fragmentShader: FRAG,
  }) as GlitchMaterial;
  mat.setTime = (seconds: number) => {
    /* uTime is defined in the uniforms map above; the optional chain
     * is only here to placate TS's "possibly undefined" check on the
     * generic uniform-value-bag type. */
    if (mat.uniforms.uTime) mat.uniforms.uTime.value = seconds;
  };
  return mat;
}
