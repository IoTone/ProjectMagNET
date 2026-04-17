import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { TEXT } from './palette';

/**
 * Shared halo + billboard label that follows the currently-hovered node.
 * One instance per scene; re-parented to the hovered node's world position each frame.
 */
export class NodeHoverFx {
  readonly group = new THREE.Group();
  private halo: THREE.Mesh;
  private label: Text;
  private target: THREE.Vector3 | null = null;
  private camera: THREE.Camera;
  private targetWorld = new THREE.Vector3();

  constructor(camera: THREE.Camera) {
    this.camera = camera;
    this.group.name = 'node-hover-fx';
    this.group.visible = false;

    this.halo = new THREE.Mesh(
      new THREE.SphereGeometry(1, 20, 20),
      new THREE.MeshBasicMaterial({
        color: TEXT.emphasis, transparent: true, opacity: 0.22,
        depthTest: false, side: THREE.BackSide,
      }),
    );
    this.halo.renderOrder = 996;
    this.halo.scale.setScalar(0.012);
    this.group.add(this.halo);

    this.label = new Text();
    this.label.text = '';
    this.label.fontSize = 0.014;
    this.label.color = TEXT.primary;
    this.label.anchorX = 'center';
    this.label.anchorY = 'bottom';
    this.label.position.set(0, 0.018, 0);
    this.label.outlineWidth = 0.001;
    this.label.outlineColor = 0x000000;
    this.label.sync();
    this.group.add(this.label);
  }

  show(worldPos: THREE.Vector3, label: string, radius = 0.012) {
    this.targetWorld.copy(worldPos);
    this.target = this.targetWorld;
    this.halo.scale.setScalar(radius * 1.8);
    this.label.text = label;
    this.label.sync();
    this.group.visible = true;
  }

  hide() {
    this.group.visible = false;
    this.target = null;
  }

  updatePosition(worldPos: THREE.Vector3) {
    this.targetWorld.copy(worldPos);
    this.target = this.targetWorld;
  }

  tick() {
    if (!this.target || !this.group.visible) return;
    this.group.position.copy(this.target);
    this.label.lookAt(this.camera.position);
  }
}
