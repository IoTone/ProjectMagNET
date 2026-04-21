import * as THREE from 'three';

export interface XRRigEvents {
  onSelectStart?: (controllerIndex: number) => void;
  onSelectEnd?: (controllerIndex: number) => void;
}

const BEAM_COLOR_IDLE = 0xffd97a;
const BEAM_COLOR_PRESS = 0xffffff;
const RETICLE_COLOR = 0xffff66;
const FINGERTIP_COLOR = 0x66ffcc;

export interface HandJointInfo {
  fingertipPos: THREE.Vector3;
  pinchDistance: number;
}

export interface WristInfo {
  position: THREE.Vector3;
  quaternion: THREE.Quaternion;
  palmNormal: THREE.Vector3;  // upward normal from the palm
}

interface HandSlot {
  controller: THREE.Group;
  grip: THREE.Group;
  hand: THREE.Group;
  beam: THREE.Mesh;
  reticle: THREE.Mesh;
  fingertipSphere: THREE.Mesh;
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

      const hand = renderer.xr.getHand(i);
      scene.add(hand);

      const beam = this.makeBeam();
      scene.add(beam);

      const reticle = this.makeReticle();
      scene.add(reticle);

      const fingertipSphere = this.makeFingertipSphere();
      scene.add(fingertipSphere);

      this.slots.push({ controller, grip, hand, beam, reticle, fingertipSphere });
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

  private makeFingertipSphere(): THREE.Mesh {
    const sphere = new THREE.Mesh(
      new THREE.SphereGeometry(0.005, 10, 10),
      new THREE.MeshBasicMaterial({
        color: FINGERTIP_COLOR, transparent: true, opacity: 0.8, depthTest: false,
      }),
    );
    sphere.renderOrder = 998;
    sphere.visible = false;
    sphere.userData.noHover = true;
    return sphere;
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

  /** Get hand-tracking joint state for a given hand (0 or 1). Returns null if joints not available. */
  getHandJointState(i: number): HandJointInfo | null {
    const slot = this.slots[i];
    if (!slot) return null;
    const hand = slot.hand as any;
    if (!hand || !hand.joints) return null;
    const indexTip = hand.joints['index-finger-tip'] as THREE.Object3D | undefined;
    const thumbTip = hand.joints['thumb-tip'] as THREE.Object3D | undefined;
    if (!indexTip || !thumbTip) return null;
    // Only trust joints that have been updated (visible in the scene)
    if (!indexTip.visible && !thumbTip.visible) return null;

    const fingertipPos = new THREE.Vector3();
    indexTip.getWorldPosition(fingertipPos);

    const thumbPos = new THREE.Vector3();
    thumbTip.getWorldPosition(thumbPos);

    const pinchDistance = fingertipPos.distanceTo(thumbPos);
    return { fingertipPos, pinchDistance };
  }

  /** Get wrist joint state for a given hand (0 or 1). Returns null if wrist joint not available. */
  getWristState(i: number): WristInfo | null {
    const slot = this.slots[i];
    if (!slot) return null;
    const hand = slot.hand as any;
    if (!hand || !hand.joints) return null;
    const wrist = hand.joints['wrist'] as THREE.Object3D | undefined;
    if (!wrist || !wrist.visible) return null;
    const position = new THREE.Vector3();
    wrist.getWorldPosition(position);
    const quaternion = new THREE.Quaternion();
    wrist.getWorldQuaternion(quaternion);
    // Palm normal: wrist's local Y axis in world space
    const palmNormal = new THREE.Vector3(0, 1, 0).applyQuaternion(quaternion);
    return { position, quaternion, palmNormal };
  }

  update(_frame: XRFrame | undefined) {
    const DEFAULT_LEN = 1.5;
    for (const slot of this.slots) {
      const { controller, grip, hand, beam, reticle, fingertipSphere } = slot;
      const source = controller.visible ? controller : (grip.visible ? grip : null);
      if (!source) {
        beam.visible = false;
        reticle.visible = false;
      } else {
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

      // Update fingertip sphere from hand-tracking joints
      const handObj = hand as any;
      if (handObj.joints) {
        const indexTip = handObj.joints['index-finger-tip'] as THREE.Object3D | undefined;
        if (indexTip && indexTip.visible) {
          const pos = new THREE.Vector3();
          indexTip.getWorldPosition(pos);
          fingertipSphere.position.copy(pos);
          fingertipSphere.visible = true;
        } else {
          fingertipSphere.visible = false;
        }
      } else {
        fingertipSphere.visible = false;
      }
    }
  }
}
