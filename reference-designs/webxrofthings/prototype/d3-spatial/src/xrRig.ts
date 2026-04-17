import * as THREE from 'three';
import { XRControllerModelFactory } from 'three/examples/jsm/webxr/XRControllerModelFactory.js';

export interface XRRigEvents {
  onSelectStart?: (controllerIndex: number) => void;
  onSelectEnd?: (controllerIndex: number) => void;
}

const BEAM_COLOR_IDLE = 0xffd97a;   // warm amber
const BEAM_COLOR_PRESS = 0xffffff;  // white on press
const RETICLE_COLOR = 0xffff66;     // bright yellow

export class XRRig {
  private renderer: THREE.WebGLRenderer;
  private scene: THREE.Scene;
  readonly controllers: THREE.Group[] = [];
  private rays: THREE.Mesh[] = [];
  private reticles: THREE.Mesh[] = [];
  private raycaster = new THREE.Raycaster();
  private tempMatrix = new THREE.Matrix4();

  constructor(renderer: THREE.WebGLRenderer, scene: THREE.Scene, private events: XRRigEvents = {}) {
    this.renderer = renderer;
    this.scene = scene;

    const modelFactory = new XRControllerModelFactory();
    for (let i = 0; i < 2; i++) {
      const controller = renderer.xr.getController(i);
      controller.addEventListener('selectstart', () => this.onSelectStart(i));
      controller.addEventListener('selectend', () => this.onSelectEnd(i));
      scene.add(controller);
      this.controllers.push(controller);

      const grip = renderer.xr.getControllerGrip(i);
      grip.add(modelFactory.createControllerModel(grip));
      scene.add(grip);

      const beamGeo = new THREE.CylinderGeometry(0.002, 0.001, 1, 10, 1, false);
      beamGeo.rotateX(-Math.PI / 2);
      beamGeo.translate(0, 0, -0.5);
      const beamMat = new THREE.MeshBasicMaterial({
        color: BEAM_COLOR_IDLE,
        transparent: true,
        opacity: 0.9,
        depthTest: false,
      });
      const beam = new THREE.Mesh(beamGeo, beamMat);
      beam.scale.z = 1.2;
      beam.renderOrder = 997;
      beam.userData.noHover = true;
      controller.add(beam);
      this.rays.push(beam);

      const reticle = new THREE.Mesh(
        new THREE.SphereGeometry(0.025, 16, 16),
        new THREE.MeshBasicMaterial({ color: RETICLE_COLOR, depthTest: false, transparent: true, opacity: 1.0 }),
      );
      reticle.renderOrder = 999;
      reticle.visible = false;
      reticle.userData.noHover = true;
      const halo = new THREE.Mesh(
        new THREE.RingGeometry(0.028, 0.034, 32),
        new THREE.MeshBasicMaterial({ color: RETICLE_COLOR, depthTest: false, transparent: true, opacity: 0.6, side: THREE.DoubleSide }),
      );
      halo.renderOrder = 998;
      halo.userData.noHover = true;
      reticle.add(halo);
      scene.add(reticle);
      this.reticles.push(reticle);
    }
  }

  private onSelectStart(i: number) {
    const ray = this.rays[i];
    if (ray) {
      const mat = ray.material as THREE.MeshBasicMaterial;
      mat.color.set(BEAM_COLOR_PRESS);
      mat.opacity = 1.0;
    }
    this.events.onSelectStart?.(i);
  }

  private onSelectEnd(i: number) {
    const ray = this.rays[i];
    if (ray) {
      const mat = ray.material as THREE.MeshBasicMaterial;
      mat.color.set(BEAM_COLOR_IDLE);
      mat.opacity = 0.9;
    }
    this.events.onSelectEnd?.(i);
  }

  update(_frame: XRFrame | undefined) {
    const DEFAULT_LEN = 1.2;
    for (let i = 0; i < this.controllers.length; i++) {
      const controller = this.controllers[i];
      const reticle = this.reticles[i];
      const ray = this.rays[i];
      if (!controller || !reticle || !ray) continue;
      if (!controller.visible) { reticle.visible = false; ray.scale.z = DEFAULT_LEN; continue; }
      this.tempMatrix.identity().extractRotation(controller.matrixWorld);
      this.raycaster.ray.origin.setFromMatrixPosition(controller.matrixWorld);
      this.raycaster.ray.direction.set(0, 0, -1).applyMatrix4(this.tempMatrix);
      const hits = this.raycaster.intersectObjects(this.scene.children, true);
      const hit = hits.find(h => !h.object.userData?.noHover && !(h.object as any).isLine);
      reticle.quaternion.copy(controller.quaternion);
      if (hit) {
        reticle.position.copy(hit.point);
        ray.scale.z = hit.distance;
        setReticleStyle(reticle, 'target');
      } else {
        const fallback = new THREE.Vector3()
          .copy(this.raycaster.ray.direction)
          .multiplyScalar(DEFAULT_LEN)
          .add(this.raycaster.ray.origin);
        reticle.position.copy(fallback);
        ray.scale.z = DEFAULT_LEN;
        setReticleStyle(reticle, 'idle');
      }
      reticle.visible = true;
    }
  }
}

function setReticleStyle(reticle: THREE.Mesh, style: 'target' | 'idle') {
  const coreMat = reticle.material as THREE.MeshBasicMaterial;
  const halo = reticle.children[0] as THREE.Mesh | undefined;
  const haloMat = halo?.material as THREE.MeshBasicMaterial | undefined;
  if (style === 'target') {
    coreMat.opacity = 1.0;
    reticle.scale.setScalar(1.0);
    if (haloMat) haloMat.opacity = 0.7;
  } else {
    coreMat.opacity = 0.45;
    reticle.scale.setScalar(0.55);
    if (haloMat) haloMat.opacity = 0.25;
  }
}
