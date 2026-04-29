import * as THREE from 'three';
const PALM_UP_THRESHOLD = 0.5;
const HIDE_DELAY_MS = 300;
export class HandMenu {
    group = new THREE.Group();
    menu;
    rig;
    headCamera;
    handIndex;
    palmUpDetected = false;
    hideTimer = null;
    _headPos = new THREE.Vector3();
    _toHead = new THREE.Vector3();
    constructor(opts) {
        this.menu = opts.menu;
        this.rig = opts.rig;
        this.headCamera = opts.headCamera;
        this.handIndex = opts.handIndex ?? 0;
        this.group.name = 'handMenu';
        this.group.add(this.menu.group);
        // Start hidden
        this.menu.hide();
    }
    /** Call each frame to update wrist position and palm-up detection */
    update() {
        const wrist = this.rig.getWristState(this.handIndex);
        if (!wrist) {
            // No hand tracking available — leave menu in its current visibility state
            return;
        }
        // Palm-up detection: check if palm normal faces roughly toward the user's head
        this.headCamera.getWorldPosition(this._headPos);
        this._toHead.copy(this._headPos).sub(wrist.position).normalize();
        const dot = wrist.palmNormal.dot(this._toHead);
        if (dot > PALM_UP_THRESHOLD) {
            // Palm facing the user — show menu at wrist
            this.palmUpDetected = true;
            if (this.hideTimer !== null) {
                this.hideTimer = null;
            }
            // Position the menu at the wrist with offset above palm
            this.group.position.copy(wrist.position);
            this.group.quaternion.copy(wrist.quaternion);
            // Offset slightly above the palm surface
            const up = new THREE.Vector3(0, 0.06, 0).applyQuaternion(wrist.quaternion);
            this.group.position.add(up);
            this.menu.show();
        }
        else if (this.palmUpDetected) {
            // Palm rotated away — start hide delay
            this.palmUpDetected = false;
            if (this.hideTimer === null) {
                this.hideTimer = performance.now();
            }
        }
        // Check hide delay
        if (this.hideTimer !== null && performance.now() - this.hideTimer > HIDE_DELAY_MS) {
            this.menu.hide();
            this.hideTimer = null;
        }
    }
}
