import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { TEXT } from './palette';
import { FONT_BLOCK_OPTS, fontColor } from './textStyles';

export interface VizHudOptions {
  onBack?: () => void;
  onReset?: () => void;
}

export class VizHud {
  readonly group = new THREE.Group();
  private backBtn: InstanceType<typeof ThreeMeshUI.Block>;
  private resetBtn: InstanceType<typeof ThreeMeshUI.Block>;
  private onBack: (() => void) | undefined;
  private onReset: (() => void) | undefined;

  constructor(opts: VizHudOptions) {
    this.onBack = opts.onBack;
    this.onReset = opts.onReset;
    this.group.name = 'vizHud';

    const btnW = 0.05;
    const btnH = 0.02;
    const gap = 0.008;

    // Back button — MSDF label inherits font context from this Block.
    this.backBtn = new ThreeMeshUI.Block({
      ...FONT_BLOCK_OPTS,
      width: btnW,
      height: btnH,
      padding: 0.003,
      borderRadius: 0.004,
      backgroundColor: new THREE.Color(0x3a2a18),
      backgroundOpacity: 0.95,
      borderWidth: 0.0012,
      borderColor: new THREE.Color(TEXT.warn),
      borderOpacity: 1.0,
      justifyContent: 'center',
      alignItems: 'center',
    });
    this.backBtn.position.set(-(btnW + gap) / 2, 0, 0);
    this.backBtn.userData.isVizHudButton = true;
    this.backBtn.userData.hudAction = 'back';
    this.backBtn.visible = false;
    this.backBtn.add(new ThreeMeshUI.Text({
      content: 'Back',
      fontSize: 0.011,
      fontColor: fontColor(TEXT.warn),
    }));
    this.group.add(this.backBtn);

    // Reset button
    this.resetBtn = new ThreeMeshUI.Block({
      ...FONT_BLOCK_OPTS,
      width: btnW,
      height: btnH,
      padding: 0.003,
      borderRadius: 0.004,
      backgroundColor: new THREE.Color(0x2a2520),
      backgroundOpacity: 0.95,
      borderWidth: 0.0012,
      borderColor: new THREE.Color(TEXT.muted),
      borderOpacity: 0.9,
      justifyContent: 'center',
      alignItems: 'center',
    });
    this.resetBtn.position.set((btnW + gap) / 2, 0, 0);
    this.resetBtn.userData.isVizHudButton = true;
    this.resetBtn.userData.hudAction = 'reset';
    this.resetBtn.add(new ThreeMeshUI.Text({
      content: 'Reset',
      fontSize: 0.011,
      fontColor: fontColor(TEXT.body),
    }));
    this.group.add(this.resetBtn);
  }

  /** Show/hide the Back button based on drill depth */
  setDrillDepth(depth: number): void {
    this.backBtn.visible = depth > 0;
  }

  /** Get blocks for Interact registration */
  getBlocks(): { block: InstanceType<typeof ThreeMeshUI.Block>; id: string; onSelect: () => void }[] {
    const blocks: { block: InstanceType<typeof ThreeMeshUI.Block>; id: string; onSelect: () => void }[] = [];
    if (this.onBack) {
      blocks.push({ block: this.backBtn, id: 'back', onSelect: this.onBack });
    }
    if (this.onReset) {
      blocks.push({ block: this.resetBtn, id: 'reset', onSelect: this.onReset });
    }
    return blocks;
  }
}
