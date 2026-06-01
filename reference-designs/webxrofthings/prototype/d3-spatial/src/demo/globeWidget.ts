/**
 * Globe widget — a small rotating three-globe Earth used as a backdrop
 * behind the Flight Info cell.
 *
 * Lives in its own module so the relatively heavy `three-globe` import
 * graph (it pulls in `three-conic-polygon-geometry`, which touches
 * `window` at module-load time) is only evaluated in the browser.
 * `liveImuCell.ts` does a dynamic `import()` gated on `typeof window`,
 * which means node-based vitest runners never resolve this file and
 * don't need to mock three-globe in their test setup.
 *
 * The OSM zoom-0 world tile we use as the globe surface is Mercator,
 * not equirectangular, so polar regions are visibly stretched when
 * fed to `globeImageUrl`. That's an acceptable trade for a single-
 * file, locally-cached, offline-friendly globe — the alternative
 * (live OSM tile fetching) introduces a runtime network dependency.
 *
 * © OpenStreetMap contributors — tile cached at `public/textures/osm-world-z0.png`.
 */
import * as THREE from 'three';
import ThreeGlobe from 'three-globe';

export interface GlobeWidgetOpts {
  /** Target world-space radius for the globe in scene units. */
  worldRadius: number;
  /** URL of the equirectangular texture to wrap the globe with. */
  imageUrl?: string;
  /** Eastward rotation per frame, in radians. */
  rotationPerFrame?: number;
}

export interface GlobeWidget {
  object: THREE.Object3D;
  update(): void;
  dispose(): void;
}

const DEFAULT_IMAGE_URL = '/textures/osm-world-z0.png';

export function buildGlobeWidget(opts: GlobeWidgetOpts): GlobeWidget {
  const {
    worldRadius,
    imageUrl = DEFAULT_IMAGE_URL,
    rotationPerFrame = 0.003,
  } = opts;

  // Unlit base material. three-globe defaults to MeshPhongMaterial; in our
  // dataspace it can end up outside the reach of the scene's hemisphere/
  // directional lights (e.g., behind the user's head when the dataspace
  // is forward), and `globeImageUrl` *also* resets the material colour
  // to black while it asynchronously loads the texture — a black globe
  // is invisible against a dark XR background. We side-step both by:
  //   1. Forcing an unlit MeshBasicMaterial.
  //   2. Skipping three-globe's `globeImageUrl` and loading the texture
  //      ourselves so the colour never flips to black, and the fallback
  //      ocean-blue stays visible if the texture fetch fails.
  const baseMaterial = new THREE.MeshBasicMaterial({ color: 0x3a6b9c });

  new THREE.TextureLoader().load(
    imageUrl,
    (texture) => {
      // sRGB encoding matches how the OSM tile PNG is authored; without
      // it the colours come out washed out / gamma-doubled depending on
      // the renderer's output encoding.
      texture.colorSpace = THREE.SRGBColorSpace;
      baseMaterial.map = texture;
      baseMaterial.needsUpdate = true;
      console.info('[globeWidget] OSM tile loaded');
    },
    undefined,
    (err) => {
      console.warn('[globeWidget] OSM tile load failed — globe stays untextured', err);
    },
  );

  // CRITICAL: `animateIn: false` is load-bearing for the embedded-cell use
  // case. With the default `animateIn: true`, three-globe's init runs a
  // 600 ms Tween that sets `state.scene.scale` from 1e-6 up to 1 — clobbering
  // any external `globe.scale` we assign afterwards. The globe then renders
  // at scale 1, i.e. its full internal radius of 100 world units, which on
  // a Quest 3 / Spectacles view fills the entire frame as a flat texture
  // wall (the OSM tile loads correctly, but you can't read it as a globe).
  //
  // `waitForGlobeReady: false` is paranoia: three-globe defaults to keeping
  // the scene hidden until an internal readiness callback fires, and our
  // minimal config (no globeImageUrl, no tile engine) hits a path where
  // the callback may not fire cleanly. Skipping the gate makes the globe
  // visible immediately.
  const globe = new ThreeGlobe({ animateIn: false, waitForGlobeReady: false })
    .globeMaterial(baseMaterial)
    .showAtmosphere(true)
    .atmosphereColor('#7fd1ff')
    .atmosphereAltitude(0.18);

  // three-globe's default radius is 100 world units. Scale the group so
  // the final on-screen radius matches `worldRadius`.
  const scale = worldRadius / globe.getGlobeRadius();
  globe.scale.setScalar(scale);

  // Visibility log — fires once the dynamic chunk evaluates, so a missing
  // globe in-headset tells us at a glance whether the chunk even loaded.
  console.info('[globeWidget] built', { worldRadius, scale, imageUrl });

  return {
    object: globe,
    update: () => { globe.rotation.y += rotationPerFrame; },
    dispose: () => {
      globe.traverse((obj) => {
        if ((obj as THREE.Mesh).isMesh) {
          const m = obj as THREE.Mesh;
          m.geometry?.dispose();
          const mats = Array.isArray(m.material) ? m.material : [m.material];
          for (const mat of mats) {
            const anyMat = mat as THREE.Material & { map?: THREE.Texture };
            anyMat.map?.dispose();
            anyMat.dispose();
          }
        }
      });
    },
  };
}
