import * as THREE from 'three';

export interface XRRigEvents {
  onSelectStart?: (controllerIndex: number) => void;
  onSelectEnd?: (controllerIndex: number) => void;
}

const BEAM_COLOR_IDLE = 0xffd97a;
const BEAM_COLOR_PRESS = 0xffffff;
const RETICLE_COLOR = 0xffff66;

interface HandSlot {
  controller: THREE.Group;
  grip: THREE.Group;
  beam: THREE.Mesh;
  reticle: THREE.Mesh;
}

export class XRRig {
  private renderer: THREE.WebGLRenderer;
  private scene: THREE.Scene;
  readonly controllers: THREE.Group[] = [];
  private slots: HandSlot[] = [];
  private raycaster = new THREE.Raycaster();
  private tempMatrix = new THREE.Matrix4();

  constructor(renderer: THREE.WebGLRenderer, scene: THREE.Scene, private events: XRRigEvents = {}) {
    this.renderer = renderer;
    this.scene = scene;

    for (let i = 0; i < 2; i++) {
      const controller = renderer.xr.getController(i);
      controller.addEventListener('selectstart', () => this.onSelectStart(i));
      controller.addEventListener('selectend', () => this.onSelectEnd(i));
      scene.add(controller);
      this.controllers.push(controller);

      const grip = renderer.xr.getControllerGrip(i);
      scene.add(grip);

      const beam = this.makeBeam();
      scene.add(beam);

      const reticle = this.makeReticle();
      scene.add(reticle);

      this.slots.push({ controller, grip, beam, reticle });
    }
  }

  private makeBeam(): THREE.Mesh {
    const geo = new THREE.CylinderGeometry(0.0005, 0.0003, 1, 6, 1, false);
    geo.rotateX(-Math.PI / 2);
    geo.translate(0, 0, -0.5);
    const mat = new THREE.MeshBasicMaterial({
      color: BEAM_COLOR_IDLE, transparent: true, opacity: 0.95, depthTest: false,
    });
    const beam = new THREE.Mesh(geo, mat);
    beam.renderOrder = 997;
    beam.userData.noHover = true;
    beam.visible = false;
    return beam;
  }

  private makeReticle(): THREE.Mesh {
    const reticle = new THREE.Mesh(
      new THREE.SphereGeometry(0.008, 12, 12),
      new THREE.MeshBasicMaterial({ color: RETICLE_COLOR, depthTest: false, transparent: true, opacity: 1.0 }),
    );
    reticle.renderOrder = 999;
    reticle.visible = false;
    reticle.userData.noHover = true;
    const halo = new THREE.Mesh(
      new THREE.RingGeometry(0.01, 0.014, 24),
      new THREE.MeshBasicMaterial({ color: RETICLE_COLOR, depthTest: false, transparent: true, opacity: 0.7, side: THREE.DoubleSide }),
    );
    halo.renderOrder = 998;
    halo.userData.noHover = true;
    reticle.add(halo);
    return reticle;
  }

  private onSelectStart(i: number) {
    const slot = this.slots[i];
    if (slot) (slot.beam.material as THREE.MeshBasicMaterial).color.set(BEAM_COLOR_PRESS);
    this.events.onSelectStart?.(i);
  }

  private onSelectEnd(i: number) {
    const slot = this.slots[i];
    if (slot) (slot.beam.material as THREE.MeshBasicMaterial).color.set(BEAM_COLOR_IDLE);
    this.events.onSelectEnd?.(i);
  }

  update(_frame: XRFrame | undefined) {
    const DEFAULT_LEN = 1.5;
    for (const slot of this.slots) {
      const { controller, grip, beam, reticle } = slot;
      const source = controller.visible ? controller : (grip.visible ? grip : null);
      if (!source) {
        beam.visible = false;
        reticle.visible = false;
        continue;
      }

      this.tempMatrix.identity().extractRotation(source.matrixWorld);
      const origin = new THREE.Vector3().setFromMatrixPosition(source.matrixWorld);
      const direction = new THREE.Vector3(0, 0, -1).applyMatrix4(this.tempMatrix);

      beam.position.copy(origin);
      beam.quaternion.setFromRotationMatrix(this.tempMatrix);
      beam.visible = true;

      this.raycaster.ray.origin.copy(origin);
      this.raycaster.ray.direction.copy(direction);
      const hits = this.raycaster.intersectObjects(this.scene.children, true);
      const hit = hits.find(h => !h.object.userData?.noHover && !(h.object as any).isLine);

      if (hit) {
        reticle.position.copy(hit.point);
        reticle.quaternion.copy(source.quaternion);
        reticle.visible = true;
        beam.scale.z = hit.distance;
      } else {
        reticle.visible = false;
        beam.scale.z = DEFAULT_LEN;
      }
    }
  }
}
