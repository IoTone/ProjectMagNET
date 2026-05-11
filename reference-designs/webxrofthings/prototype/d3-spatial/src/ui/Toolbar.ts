import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { TEXT } from './palette';
import { FONT_BLOCK_OPTS, fontColor, sanitizeText } from './textStyles';

export interface ToolbarButton {
  id: string;
  label: string;
  block: any;
  onSelect: () => void;
}

export interface ToolbarOptions {
  buttons: Array<{ id: string; label: string; onSelect: () => void; active?: boolean }>;
  width?: number;
  height?: number;
}

export class Toolbar {
  readonly group = new THREE.Group();
  readonly buttons: ToolbarButton[] = [];

  constructor(opts: ToolbarOptions) {
    const { buttons } = opts;
    this.group.name = 'toolbar';

    const btnW = 0.08;
    const btnH = 0.028;
    const gap = 0.008;
    const total = buttons.length * btnW + (buttons.length - 1) * gap;
    let x = -total / 2 + btnW / 2;

    for (const b of buttons) {
      // Block is now a flex container that centers its Text child — no more
      // absolute-position math for the label. Font URLs come from the shared
      // textStyles helper so we don't sprinkle the MSDF paths through 7 files.
      const block = new ThreeMeshUI.Block({
        ...FONT_BLOCK_OPTS,
        width: btnW,
        height: btnH,
        padding: 0.004,
        borderRadius: 0.006,
        backgroundColor: new THREE.Color(b.active ? 0x3a4a28 : 0x2a2520),
        backgroundOpacity: 0.92,
        borderWidth: 0.0012,
        borderColor: new THREE.Color(b.active ? 0x66ccff : TEXT.muted),
        borderOpacity: 0.9,
        justifyContent: 'center',
        alignItems: 'center',
      });
      block.position.set(x, 0, 0);
      block.userData.isToolbarButton = true;
      block.userData.buttonId = b.id;
      x += btnW + gap;

      block.add(new ThreeMeshUI.Text({
        content: sanitizeText(b.label),
        fontSize: 0.011,
        fontColor: fontColor(b.active ? TEXT.emphasis : TEXT.body),
      }));

      this.group.add(block);
      this.buttons.push({ id: b.id, label: b.label, block, onSelect: b.onSelect });
    }
  }

  setActive(id: string) {
    for (const b of this.buttons) {
      const active = b.id === id;
      b.block.set({
        backgroundColor: new THREE.Color(active ? 0x3a4a28 : 0x2a2520),
        borderColor: new THREE.Color(active ? 0x66ccff : TEXT.muted),
      });
    }
  }

  triggerById(id: string) {
    const b = this.buttons.find(x => x.id === id);
    b?.onSelect();
  }
}
