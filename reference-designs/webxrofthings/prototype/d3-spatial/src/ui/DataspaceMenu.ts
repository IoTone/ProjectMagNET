import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { TEXT } from './palette';
import type { DataspaceHudItem } from '../manifest/schema';

export interface DataspaceMenuOptions {
  items: DataspaceHudItem[];
  onAction: (actionId: string) => void;
  position?: 'bottom' | 'side' | 'wrist';
}

interface MenuItemEntry {
  item: DataspaceHudItem;
  block: InstanceType<typeof ThreeMeshUI.Block>;
  iconText: Text;
  labelText: Text;
}

const BG_DEFAULT = 0x2a2520;
const BG_HOVER = 0x3a3228;
const ITEM_W = 0.12;
const ITEM_H = 0.025;
const ITEM_GAP = 0.004;

export class DataspaceMenu {
  readonly group = new THREE.Group();
  private entries: MenuItemEntry[] = [];
  private onAction: (actionId: string) => void;
  private position: 'bottom' | 'side' | 'wrist';

  constructor(opts: DataspaceMenuOptions) {
    this.group.name = 'dataspaceMenu';
    this.onAction = opts.onAction;
    this.position = opts.position ?? 'bottom';
    this.buildItems(opts.items);
  }

  private buildItems(items: DataspaceHudItem[]): void {
    // Clear existing
    for (const e of this.entries) {
      this.group.remove(e.block);
    }
    this.entries = [];

    const totalH = items.length * ITEM_H + (items.length - 1) * ITEM_GAP;
    let y = totalH / 2 - ITEM_H / 2;

    for (const item of items) {
      const block = new ThreeMeshUI.Block({
        width: ITEM_W,
        height: ITEM_H,
        padding: 0.003,
        borderRadius: 0.005,
        backgroundColor: new THREE.Color(BG_DEFAULT),
        backgroundOpacity: 0.92,
        borderWidth: 0.001,
        borderColor: new THREE.Color(TEXT.muted),
        borderOpacity: 0.7,
      });
      block.position.set(0, y, 0);
      block.userData.isDataspaceMenuButton = true;
      block.userData.menuItemId = item.id;
      block.userData.menuAction = item.action;
      y -= ITEM_H + ITEM_GAP;

      // Icon (emoji) text — left side
      const iconText = new Text();
      iconText.text = item.icon ?? '';
      iconText.fontSize = 0.012;
      iconText.color = TEXT.body;
      iconText.anchorX = 'center';
      iconText.anchorY = 'middle';
      iconText.position.set(-ITEM_W / 2 + 0.014, 0, 0.002);
      iconText.sync();
      block.add(iconText);

      // Label text — center/right
      const labelText = new Text();
      labelText.text = item.label;
      labelText.fontSize = 0.01;
      labelText.color = TEXT.body;
      labelText.anchorX = 'left';
      labelText.anchorY = 'middle';
      labelText.position.set(-ITEM_W / 2 + 0.028, 0, 0.002);
      labelText.sync();
      block.add(labelText);

      this.group.add(block);
      this.entries.push({ item, block, iconText, labelText });
    }
  }

  /** Get blocks for Interact registration */
  getBlocks(): Array<{ block: InstanceType<typeof ThreeMeshUI.Block>; id: string; onSelect: () => void }> {
    return this.entries.map(e => ({
      block: e.block,
      id: `dsmenu:${e.item.id}`,
      onSelect: () => this.onAction(e.item.action),
    }));
  }

  /** Update the menu to reflect new items (e.g., after manifest reload) */
  setItems(items: DataspaceHudItem[]): void {
    this.buildItems(items);
  }

  /** Show the menu */
  show(): void {
    this.group.visible = true;
  }

  /** Hide the menu */
  hide(): void {
    this.group.visible = false;
  }

  /** For wrist mode: update position from hand joints */
  updateWristPosition(wristMatrix: THREE.Matrix4): void {
    this.group.position.setFromMatrixPosition(wristMatrix);
    const q = new THREE.Quaternion();
    q.setFromRotationMatrix(wristMatrix);
    this.group.quaternion.copy(q);
    // Offset slightly above the palm
    const up = new THREE.Vector3(0, 0.06, 0).applyQuaternion(q);
    this.group.position.add(up);
  }

  /** Apply hover-in styling to a block */
  hoverIn(block: InstanceType<typeof ThreeMeshUI.Block>): void {
    (block as any).set({
      backgroundColor: new THREE.Color(BG_HOVER),
      backgroundOpacity: 1.0,
    });
  }

  /** Apply hover-out styling to a block */
  hoverOut(block: InstanceType<typeof ThreeMeshUI.Block>): void {
    (block as any).set({
      backgroundColor: new THREE.Color(BG_DEFAULT),
      backgroundOpacity: 0.92,
    });
  }

  /** Get the configured position mode */
  getPosition(): 'bottom' | 'side' | 'wrist' {
    return this.position;
  }
}
