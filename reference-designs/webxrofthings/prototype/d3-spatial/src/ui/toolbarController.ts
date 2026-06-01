/**
 * toolbarController — owns the toolbar's scene attach, per-button Interact
 * registration (hover-color tint + select callback), and "place in front of
 * the user" math. Lifted out of main.ts so the orchestrator just declares
 * buttons; this controller wires them up.
 *
 * The underlying `Toolbar` (src/ui/Toolbar.ts) is unchanged. The controller
 * exposes a thin facade — `toolbar` for `setActive(id)`, `placeNearUser()`
 * for the "follow the user" behaviour the toolbar already had.
 */

import * as THREE from 'three';
import { Toolbar, type ToolbarOptions } from './Toolbar';
import type { Interact } from '../interact/Interact';

/** Input shape the Toolbar constructor accepts (button block is created internally). */
export type ToolbarButtonInput = ToolbarOptions['buttons'][number];

export interface ToolbarController {
  /** The underlying Toolbar instance — exposed for `setActive(id)` and any
   *  future caller that needs to peek at button blocks. */
  toolbar: Toolbar;
  /** Re-position the toolbar 0.8 m in front of the user, slightly below eye-line.
   *  Call after `recenter` actions, scene changes, etc. */
  placeNearUser(): void;
}

export interface ToolbarControllerOptions {
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  renderer: THREE.WebGLRenderer;
  interact: Interact;
  buttons: ToolbarButtonInput[];
}

const HOVER_BG    = new THREE.Color(0x2a4a6a);
const HOVER_ALPHA = 1.0;
const REST_ALPHA  = 0.92;
const FALLBACK_BG = 0x0f1a2c;
const TOOLBAR_DISTANCE_M = 0.8;
const TOOLBAR_Y_OFFSET   = -0.35;

export function createToolbarController(opts: ToolbarControllerOptions): ToolbarController {
  const { scene, camera, renderer, interact, buttons } = opts;

  const toolbar = new Toolbar({ buttons });
  scene.add(toolbar.group);

  for (const btn of toolbar.buttons) {
    const origBg = btn.block.backgroundColor?.clone?.() ?? new THREE.Color(FALLBACK_BG);
    interact.add({
      id: `btn:${btn.id}`,
      object: btn.block,
      onHoverIn: () => {
        btn.block.set({ backgroundColor: HOVER_BG, backgroundOpacity: HOVER_ALPHA });
      },
      onHoverOut: () => {
        btn.block.set({ backgroundColor: origBg, backgroundOpacity: REST_ALPHA });
      },
      onSelect: () => btn.onSelect(),
    });
  }

  function placeNearUser() {
    const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
    const pos = new THREE.Vector3();
    xrCam.getWorldPosition(pos);
    const fwd = new THREE.Vector3(0, 0, -1).applyQuaternion(xrCam.quaternion);
    fwd.y = 0;
    if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
    fwd.normalize();
    toolbar.group.position.set(
      pos.x + fwd.x * TOOLBAR_DISTANCE_M,
      pos.y + TOOLBAR_Y_OFFSET,
      pos.z + fwd.z * TOOLBAR_DISTANCE_M,
    );
    toolbar.group.lookAt(pos.x, toolbar.group.position.y, pos.z);
  }

  placeNearUser();
  return { toolbar, placeNearUser };
}
