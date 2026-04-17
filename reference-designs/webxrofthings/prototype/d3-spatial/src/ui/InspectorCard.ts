import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { TEXT } from './palette';

export interface InspectorContent {
  title: string;
  subtitle?: string;
  value?: string;
}

export class InspectorCard {
  readonly block: any;
  private title: Text;
  private subtitle: Text;
  private value: Text;

  constructor() {
    this.block = new ThreeMeshUI.Block({
      width: 0.18,
      height: 0.085,
      padding: 0.01,
      borderRadius: 0.008,
      backgroundColor: new THREE.Color(0x0f1a2c),
      backgroundOpacity: 0.92,
      borderWidth: 0.0012,
      borderColor: new THREE.Color(0x3a5a8a),
      borderOpacity: 0.9,
    });
    this.block.name = 'inspector-card';
    this.block.visible = false;

    this.title = new Text();
    this.title.fontSize = 0.012;
    this.title.color = TEXT.primary;
    this.title.anchorX = 'left';
    this.title.anchorY = 'top';
    this.title.position.set(-0.08, 0.03, 0.002);

    this.subtitle = new Text();
    this.subtitle.fontSize = 0.0095;
    this.subtitle.color = TEXT.muted;
    this.subtitle.anchorX = 'left';
    this.subtitle.anchorY = 'top';
    this.subtitle.position.set(-0.08, 0.012, 0.002);

    this.value = new Text();
    this.value.fontSize = 0.016;
    this.value.color = TEXT.emphasis;
    this.value.anchorX = 'left';
    this.value.anchorY = 'top';
    this.value.position.set(-0.08, -0.006, 0.002);

    this.block.add(this.title);
    this.block.add(this.subtitle);
    this.block.add(this.value);
  }

  show(content: InspectorContent) {
    this.title.text = content.title;
    this.subtitle.text = content.subtitle ?? '';
    this.value.text = content.value ?? '';
    this.title.sync();
    this.subtitle.sync();
    this.value.sync();
    this.block.visible = true;
    this.title.visible = true;
    this.subtitle.visible = true;
    this.value.visible = true;
  }

  hide() {
    this.block.visible = false;
    this.title.visible = false;
    this.subtitle.visible = false;
    this.value.visible = false;
  }

  placeNear(
    target: THREE.Object3D,
    opts: { preferredSide?: 'auto' | 'left' | 'right' | 'above' | 'below'; gap?: number } = {},
  ) {
    const { preferredSide = 'auto', gap = 0.02 } = opts;
    const box = new THREE.Box3().setFromObject(target);
    const center = box.getCenter(new THREE.Vector3());
    const size = box.getSize(new THREE.Vector3());
    const parent = this.block.parent as THREE.Object3D | null;
    const localCenter = center.clone();
    if (parent) parent.worldToLocal(localCenter);

    const cardW = 0.18;
    const cardH = 0.085;

    let side = preferredSide;
    if (side === 'auto') {
      side = localCenter.x < 0 ? 'right' : 'left';
    }

    let dx = 0;
    let dy = 0;
    if (side === 'right') dx = size.x / 2 + gap + cardW / 2;
    else if (side === 'left') dx = -(size.x / 2 + gap + cardW / 2);
    else if (side === 'above') dy = size.y / 2 + gap + cardH / 2;
    else if (side === 'below') dy = -(size.y / 2 + gap + cardH / 2);

    this.block.position.set(localCenter.x + dx, localCenter.y + dy, localCenter.z + 0.01);
  }
}
