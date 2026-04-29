import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { TEXT } from './palette';
export class Breadcrumb {
    group = new THREE.Group();
    crumbs = [];
    separators = [];
    onNavigate;
    constructor(opts) {
        this.onNavigate = opts.onNavigate;
        this.group.name = 'breadcrumb';
        // Start with just root
        this.setPath(['root']);
    }
    /** Update the trail. labels[0] = root, labels[n] = current focus. */
    setPath(labels) {
        // Remove old crumbs and separators
        for (const c of this.crumbs) {
            this.group.remove(c.block);
        }
        for (const s of this.separators) {
            this.group.remove(s);
        }
        this.crumbs = [];
        this.separators = [];
        const crumbW = 0.06;
        const crumbH = 0.02;
        const sepW = 0.015;
        const gap = 0.004;
        const totalW = labels.length * crumbW + (labels.length - 1) * (sepW + gap * 2);
        let x = -totalW / 2 + crumbW / 2;
        for (let i = 0; i < labels.length; i++) {
            const isLast = i === labels.length - 1;
            const isRoot = labels.length === 1;
            const block = new ThreeMeshUI.Block({
                width: crumbW,
                height: crumbH,
                padding: 0.002,
                borderRadius: 0.004,
                backgroundColor: new THREE.Color(isLast ? 0x3a4a28 : 0x2a2520),
                backgroundOpacity: 0.92,
                borderWidth: 0.0012,
                borderColor: new THREE.Color(isLast ? TEXT.primary : TEXT.muted),
                borderOpacity: isLast ? 1.0 : 0.7,
            });
            block.position.set(x, 0, 0);
            block.userData.isBreadcrumb = true;
            block.userData.crumbIndex = i;
            const text = new Text();
            text.text = labels[i];
            text.fontSize = 0.011;
            text.color = isRoot && labels.length === 1 ? TEXT.dim : (isLast ? TEXT.primary : TEXT.muted);
            text.anchorX = 'center';
            text.anchorY = 'middle';
            text.position.set(0, 0, 0.002);
            text.sync();
            block.add(text);
            this.group.add(block);
            this.crumbs.push({ block, text, label: labels[i] });
            x += crumbW / 2;
            // Add separator after all but last
            if (i < labels.length - 1) {
                x += gap;
                const sep = new Text();
                sep.text = '>';
                sep.fontSize = 0.009;
                sep.color = TEXT.dim;
                sep.anchorX = 'center';
                sep.anchorY = 'middle';
                sep.position.set(x + sepW / 2, 0, 0.001);
                sep.sync();
                this.group.add(sep);
                this.separators.push(sep);
                x += sepW + gap;
                x += crumbW / 2;
            }
        }
    }
    /** Get the clickable blocks for Interact registration */
    getBlocks() {
        return this.crumbs.map((c, i) => ({ block: c.block, index: i }));
    }
    /** Get the onNavigate callback */
    navigate(depthIndex) {
        this.onNavigate(depthIndex);
    }
}
