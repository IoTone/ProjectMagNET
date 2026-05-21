/*
 * moonPhasesArc — small-multiples of one lunar cycle, arced in 3D so
 * the user is wrapped from waxing crescent (front-right) around to full
 * (behind) to waning crescent (front-left). Grand-scale "art" rendering
 * intended for the UC3 XRt Exhibit.
 *
 * Reference: https://observablehq.com/@mbostock/phases-of-the-moon
 *
 * Each moon is a sphere with a phase-aware shader: a directional light
 * vector L is rotated around the moon's local Y axis by the cycle's
 * phase angle; the lit hemisphere is shaded ~white, the dark hemisphere
 * ~rim-glow-dark. No textures, no external assets.
 *
 * The cell self-positions in world coordinates: the manifest just adds
 * the group at origin and renderManifest skips the standard grid slot
 * (see renderManifest.ts's `SELF_POSITIONED` set).
 */

import * as THREE from 'three';

export interface MoonPhasesArcOptions {
  /** Number of moons across the arc. Default 29 (one synodic month). */
  count?: number;
  /** Arc sweep in degrees. 360 = full circle behind+front; 270 leaves
   *  a 90° gap behind. Default 300 — close to a full wrap but with a
   *  small "stage exit" gap directly behind the user. */
  arcDegrees?: number;
  /** Radius in metres. Default 2.6 — comfortably outside a seated reach
   *  envelope, close enough that each moon reads as object-sized. */
  radius?: number;
  /** Moon diameter in metres. Default 0.30. */
  moonSize?: number;
  /** Y position of the moon row centre. Default 1.6 (slightly above
   *  standing eye-level). */
  height?: number;
  /** Roll the cycle so phase 0 (new moon) sits at this clock angle in
   *  degrees, 0=front, 90=right, 180=behind. Default 0 (new moon front-
   *  centre, full moon behind the user). */
  rolldegrees?: number;
}

export interface MoonPhasesArcViz {
  group: THREE.Group;
  dispose(): void;
}

/** Custom shader material — illuminated half / shadow half based on a
 *  phase angle. The terminator runs along the moon's local meridian
 *  rotated by uPhase radians. */
function makeMoonMaterial(phase: number): THREE.ShaderMaterial {
  return new THREE.ShaderMaterial({
    uniforms: {
      uPhase:    { value: phase },
      uLitColor: { value: new THREE.Color(0xfaf6e3) },  /* warm ivory */
      uDarkColor:{ value: new THREE.Color(0x1a1a22) },  /* deep slate */
      uRimColor: { value: new THREE.Color(0x3a3850) },  /* faint rim glow on the dark limb */
    },
    vertexShader: /* glsl */ `
      varying vec3 vNormalLocal;
      void main() {
        vNormalLocal = normalize(normal);
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
      }
    `,
    fragmentShader: /* glsl */ `
      uniform float uPhase;
      uniform vec3  uLitColor;
      uniform vec3  uDarkColor;
      uniform vec3  uRimColor;
      varying vec3  vNormalLocal;
      void main() {
        /* Sun direction rotates around the moon's local Y axis by uPhase.
         * uPhase = 0     → sun from +X  → right hemisphere lit (waxing crescent)
         * uPhase = π/2   → sun from +Z  → near hemisphere lit (first quarter)
         * uPhase = π     → sun from -X  → opposite hemisphere lit (full)
         * uPhase = 3π/2  → sun from -Z  → far hemisphere lit (last quarter)
         * Matches the synodic cycle as the viewer sweeps the arc. */
        vec3 sunDir = vec3(cos(uPhase), 0.0, sin(uPhase));
        float ndl = dot(vNormalLocal, sunDir);
        /* Soft terminator: smoothstep instead of hard step. */
        float lit = smoothstep(-0.05, 0.05, ndl);
        vec3 base = mix(uDarkColor, uLitColor, lit);
        /* Subtle rim on the dark side so the silhouette never disappears
         * against a dark scene background. */
        float rim = 1.0 - abs(ndl);
        rim = pow(rim, 6.0) * (1.0 - lit) * 0.35;
        gl_FragColor = vec4(base + uRimColor * rim, 1.0);
      }
    `,
  });
}

export function buildMoonPhasesArc(opts: MoonPhasesArcOptions = {}): MoonPhasesArcViz {
  const {
    count       = 29,
    arcDegrees  = 300,
    radius      = 2.6,
    moonSize    = 0.30,
    height      = 1.6,
    rolldegrees = 0,
  } = opts;

  const group = new THREE.Group();
  group.name = 'moon-phases-arc';

  const arcRad  = THREE.MathUtils.degToRad(arcDegrees);
  const rollRad = THREE.MathUtils.degToRad(rolldegrees);
  /* Distribute the arc symmetrically around `rollRad` so the centre moon
   * sits directly in front when arcDegrees < 360 and rolldegrees = 0. */
  const startAngle = rollRad - arcRad / 2;
  const step       = count > 1 ? arcRad / (count - 1) : 0;

  const sphereGeo = new THREE.SphereGeometry(moonSize / 2, 32, 24);
  const meshes: THREE.Mesh[]            = [];
  const mats:   THREE.ShaderMaterial[]  = [];

  for (let i = 0; i < count; i++) {
    /* Position around the arc. Angle convention: 0 = +Z (front of user),
     * positive = clockwise looking down (i.e. toward +X on the right).
     * That matches the camera's default look-down-+Z so the front moon
     * sits in front of a centred user. */
    const a  = startAngle + i * step;
    const px = Math.sin(a) * radius;
    const pz = Math.cos(a) * radius;

    /* Phase per moon: 0..2π across the cycle. The first moon is the new
     * moon (sun behind, dark face toward viewer); the centre moon is the
     * full (sun in front, lit face toward viewer). */
    const phase = (i / count) * 2 * Math.PI;
    const mat   = makeMoonMaterial(phase);

    const mesh  = new THREE.Mesh(sphereGeo, mat);
    mesh.position.set(px, height, pz);
    /* Face the user (origin). Without this the terminator's local axis
     * isn't oriented relative to the viewer and the arc reads as a row
     * of randomly-lit balls. */
    mesh.lookAt(0, height, 0);

    group.add(mesh);
    meshes.push(mesh);
    mats.push(mat);
  }

  return {
    group,
    dispose() {
      sphereGeo.dispose();
      for (const m of mats) m.dispose();
      for (const mesh of meshes) group.remove(mesh);
    },
  };
}
