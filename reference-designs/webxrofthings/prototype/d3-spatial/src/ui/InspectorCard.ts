import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { TEXT } from './palette';
import { FONT_BLOCK_OPTS, fontColor } from './textStyles';

export interface InspectorContent {
  title: string;
  subtitle?: string;
  value?: string;
}

/** Invisible Block slot for a single left-anchored text line within the card. */
function makeLeftSlot(width: number, height: number) {
  return new ThreeMeshUI.Block({
    width, height,
    backgroundOpacity: 0,
    borderOpacity: 0,
    justifyContent: 'center',
    alignItems: 'start',
    textAlign: 'left',
  });
}

export class InspectorCard {
  readonly block: any;
  private titleText: any;
  private subtitleText: any;
  private valueText: any;
  private titleSlot: any;
  private subtitleSlot: any;
  private valueSlot: any;

  constructor() {
    this.block = new ThreeMeshUI.Block({
      ...FONT_BLOCK_OPTS,
      width: 0.18,
      height: 0.085,
      padding: 0.01,
      borderRadius: 0.008,
      backgroundColor: new THREE.Color(0x2a2520),
      backgroundOpacity: 0.92,
      borderWidth: 0.0012,
      borderColor: new THREE.Color(0xb8a380),
      borderOpacity: 0.9,
    });
    this.block.name = 'inspector-card';
    this.block.visible = false;

    // Three stacked left-aligned slots — same y positions as the prior troika
    // anchors, but each text now lives inside its own MSDF-aware Block.
    this.titleSlot = makeLeftSlot(0.16, 0.020);
    this.titleSlot.position.set(0, 0.025, 0.002);
    this.block.add(this.titleSlot);
    this.titleText = new ThreeMeshUI.Text({
      content: '', fontSize: 0.012, fontColor: fontColor(TEXT.primary),
    });
    this.titleSlot.add(this.titleText);

    this.subtitleSlot = makeLeftSlot(0.16, 0.016);
    this.subtitleSlot.position.set(0, 0.005, 0.002);
    this.block.add(this.subtitleSlot);
    this.subtitleText = new ThreeMeshUI.Text({
      content: '', fontSize: 0.0095, fontColor: fontColor(TEXT.muted),
    });
    this.subtitleSlot.add(this.subtitleText);

    this.valueSlot = makeLeftSlot(0.16, 0.024);
    this.valueSlot.position.set(0, -0.020, 0.002);
    this.block.add(this.valueSlot);
    this.valueText = new ThreeMeshUI.Text({
      content: '', fontSize: 0.016, fontColor: fontColor(TEXT.emphasis),
    });
    this.valueSlot.add(this.valueText);
  }

  show(content: InspectorContent) {
    this.titleText.set({ content: content.title });
    this.subtitleText.set({ content: content.subtitle ?? '' });
    this.valueText.set({ content: content.value ?? '' });
    this.block.visible = true;
  }

  hide() {
    this.block.visible = false;
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
