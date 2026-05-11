import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { TEXT } from './palette';
import { FONT_BLOCK_OPTS, fontColor, sanitizeText } from './textStyles';
import type { DataspaceHudItem } from '../manifest/schema';

export interface DataspaceMenuOptions {
  items: DataspaceHudItem[];
  onAction: (actionId: string) => void;
  position?: 'bottom' | 'side' | 'wrist';
}

interface MenuItemEntry {
  item: DataspaceHudItem;
  block: InstanceType<typeof ThreeMeshUI.Block>;
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
        ...FONT_BLOCK_OPTS,
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

      // Icon slot (left, centered) + label slot (left of center) — each a
      // transparent Block carrying the font opts explicitly so the MSDF
      // Text inside doesn't depend on ancestor inheritance.
      //
      // Manifest icons are usually emoji (lock / refresh / etc.) which the
      // bundled Roboto-msdf doesn't cover; sanitizeText drops them silently.
      // A future polish pass can introduce single-letter "icon glyphs" so
      // the column still visually differentiates rows.
      const iconSlot = new ThreeMeshUI.Block({
        ...FONT_BLOCK_OPTS,
        width: 0.018, height: ITEM_H,
        backgroundOpacity: 0, borderOpacity: 0,
        justifyContent: 'center', alignItems: 'center',
      });
      iconSlot.position.set(-ITEM_W / 2 + 0.014, 0, 0.002);
      block.add(iconSlot);
      iconSlot.add(new ThreeMeshUI.Text({
        content: sanitizeText(item.icon ?? ''),
        fontSize: 0.012,
        fontColor: fontColor(TEXT.body),
      }));

      const labelSlot = new ThreeMeshUI.Block({
        ...FONT_BLOCK_OPTS,
        width: ITEM_W - 0.032, height: ITEM_H,
        backgroundOpacity: 0, borderOpacity: 0,
        justifyContent: 'center', alignItems: 'start',
        textAlign: 'left',
      });
      labelSlot.position.set(0.012, 0, 0.002);
      block.add(labelSlot);
      labelSlot.add(new ThreeMeshUI.Text({
        content: sanitizeText(item.label),
        fontSize: 0.01,
        fontColor: fontColor(TEXT.body),
      }));

      this.group.add(block);
      this.entries.push({ item, block });
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
