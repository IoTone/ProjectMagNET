import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { TEXT } from './palette';
export class VizHud {
    group = new THREE.Group();
    backBtn;
    resetBtn;
    backText;
    onBack;
    onReset;
    constructor(opts) {
        this.onBack = opts.onBack;
        this.onReset = opts.onReset;
        this.group.name = 'vizHud';
        const btnW = 0.05;
        const btnH = 0.02;
        const gap = 0.008;
        // Back button
        this.backBtn = new ThreeMeshUI.Block({
            width: btnW,
            height: btnH,
            padding: 0.003,
            borderRadius: 0.004,
            backgroundColor: new THREE.Color(0x3a2a18),
            backgroundOpacity: 0.95,
            borderWidth: 0.0012,
            borderColor: new THREE.Color(TEXT.warn),
            borderOpacity: 1.0,
        });
        this.backBtn.position.set(-(btnW + gap) / 2, 0, 0);
        this.backBtn.userData.isVizHudButton = true;
        this.backBtn.userData.hudAction = 'back';
        this.backBtn.visible = false;
        this.backText = new Text();
        this.backText.text = 'Back';
        this.backText.fontSize = 0.011;
        this.backText.color = TEXT.warn;
        this.backText.anchorX = 'center';
        this.backText.anchorY = 'middle';
        this.backText.position.set(0, 0, 0.002);
        this.backText.sync();
        this.backBtn.add(this.backText);
        this.group.add(this.backBtn);
        // Reset button
        this.resetBtn = new ThreeMeshUI.Block({
            width: btnW,
            height: btnH,
            padding: 0.003,
            borderRadius: 0.004,
            backgroundColor: new THREE.Color(0x2a2520),
            backgroundOpacity: 0.95,
            borderWidth: 0.0012,
            borderColor: new THREE.Color(TEXT.muted),
            borderOpacity: 0.9,
        });
        this.resetBtn.position.set((btnW + gap) / 2, 0, 0);
        this.resetBtn.userData.isVizHudButton = true;
        this.resetBtn.userData.hudAction = 'reset';
        const resetText = new Text();
        resetText.text = 'Reset';
        resetText.fontSize = 0.011;
        resetText.color = TEXT.body;
        resetText.anchorX = 'center';
        resetText.anchorY = 'middle';
        resetText.position.set(0, 0, 0.002);
        resetText.sync();
        this.resetBtn.add(resetText);
        this.group.add(this.resetBtn);
    }
    /** Show/hide the Back button based on drill depth */
    setDrillDepth(depth) {
        this.backBtn.visible = depth > 0;
    }
    /** Get blocks for Interact registration */
    getBlocks() {
        const blocks = [];
        if (this.onBack) {
            blocks.push({ block: this.backBtn, id: 'back', onSelect: this.onBack });
        }
        if (this.onReset) {
            blocks.push({ block: this.resetBtn, id: 'reset', onSelect: this.onReset });
        }
        return blocks;
    }
}
