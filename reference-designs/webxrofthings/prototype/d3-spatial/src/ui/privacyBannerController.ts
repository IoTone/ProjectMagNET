/**
 * privacyBannerController — owns the privacy banner's lifetime, anchor
 * placement, and Interact wiring. Lifted out of main.ts so the orchestrator
 * doesn't carry banner-specific scene + interact details.
 *
 * The banner itself (visuals, layout, accept button) lives in PrivacyBanner.ts
 * and is unchanged. This controller only handles:
 *   - first-time scene attach + interact registration
 *   - re-skin per dataspace (call `attach` on each manifest entry)
 *   - "in-front-of-user" placement when auto-showing
 *   - `show-privacy` HUD action re-open path (callers use `show()`)
 */

import * as THREE from 'three';
import type { DataspaceManifest } from '../manifest/schema';
import type { Interact } from '../interact/Interact';
import {
  createPrivacyBanner,
  privacyFactsFromManifest,
  type PrivacyBanner,
} from './PrivacyBanner';

export interface PrivacyBannerController {
  /** Re-skin facts for `manifest`. If `autoShow` is true and the manifest is
   *  tagged `health-data` on any device, the banner pops up in front of the
   *  user; otherwise stays hidden. */
  attach(manifest: DataspaceManifest, autoShow: boolean): void;
  /** Re-show the banner (e.g. from a `show-privacy` HUD action). No-op if
   *  no manifest has been attached yet. */
  show(): void;
  hide(): void;
  /** True when the banner is currently visible. */
  visible(): boolean;
}

export interface PrivacyBannerControllerOptions {
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  renderer: THREE.WebGLRenderer;
  interact: Interact;
}

export function createPrivacyBannerController(
  opts: PrivacyBannerControllerOptions,
): PrivacyBannerController {
  const { scene, camera, renderer, interact } = opts;

  const anchor = new THREE.Group();
  anchor.name = 'privacyBannerAnchor';
  scene.add(anchor);

  let banner: PrivacyBanner | null = null;
  let attached = false;

  function placeInFrontOfUser() {
    const xrCam = renderer.xr.isPresenting ? renderer.xr.getCamera() : camera;
    const pos = new THREE.Vector3();
    xrCam.getWorldPosition(pos);
    const fwd = new THREE.Vector3(0, 0, -1).applyQuaternion(xrCam.quaternion);
    fwd.y = 0;
    if (fwd.lengthSq() < 1e-4) fwd.set(0, 0, -1);
    fwd.normalize();
    anchor.position.set(pos.x + fwd.x * 1.0, pos.y - 0.05, pos.z + fwd.z * 1.0);
    anchor.lookAt(pos.x, anchor.position.y, pos.z);
  }

  function ensureBuilt() {
    if (banner) return;
    banner = createPrivacyBanner();
    anchor.add(banner.group);
    interact.add({
      id: banner.acceptId,
      object: banner.acceptButton,
      onHoverIn: () => {
        banner!.acceptButton.set({
          backgroundColor: new THREE.Color(0x6a4540),
          backgroundOpacity: 1.0,
        });
      },
      onHoverOut: () => {
        banner!.acceptButton.set({
          backgroundColor: new THREE.Color(0x4a3530),
          backgroundOpacity: 0.92,
        });
      },
      onSelect: () => banner!.hide(),
    });
  }

  return {
    attach(manifest, autoShow) {
      ensureBuilt();
      const facts = privacyFactsFromManifest(manifest);
      banner!.setFacts(facts);
      attached = true;
      if (autoShow && facts.hasHealthData) {
        placeInFrontOfUser();
        banner!.show();
      }
    },
    show() {
      if (!banner || !attached) return;
      placeInFrontOfUser();
      banner.show();
    },
    hide() {
      banner?.hide();
    },
    visible() {
      return banner?.visible() ?? false;
    },
  };
}
