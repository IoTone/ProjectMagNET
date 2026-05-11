import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { TEXT } from './palette';
import { FONT_BLOCK_OPTS, fontColor } from './textStyles';

export interface BreadcrumbOptions {
  onNavigate: (depthIndex: number) => void;
}

interface CrumbEntry {
  block: InstanceType<typeof ThreeMeshUI.Block>;
  label: string;
}

export class Breadcrumb {
  readonly group = new THREE.Group();
  private crumbs: CrumbEntry[] = [];
  private separators: InstanceType<typeof ThreeMeshUI.Block>[] = [];
  private onNavigate: (depthIndex: number) => void;

  constructor(opts: BreadcrumbOptions) {
    this.onNavigate = opts.onNavigate;
    this.group.name = 'breadcrumb';
    // Start with just root
    this.setPath(['root']);
  }

  /** Update the trail. labels[0] = root, labels[n] = current focus. */
  setPath(labels: string[]): void {
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
      const labelColor = isRoot && labels.length === 1 ? TEXT.dim : (isLast ? TEXT.primary : TEXT.muted);

      const block = new ThreeMeshUI.Block({
        ...FONT_BLOCK_OPTS,
        width: crumbW,
        height: crumbH,
        padding: 0.002,
        borderRadius: 0.004,
        backgroundColor: new THREE.Color(isLast ? 0x3a4a28 : 0x2a2520),
        backgroundOpacity: 0.92,
        borderWidth: 0.0012,
        borderColor: new THREE.Color(isLast ? TEXT.primary : TEXT.muted),
        borderOpacity: isLast ? 1.0 : 0.7,
        justifyContent: 'center',
        alignItems: 'center',
      });
      block.position.set(x, 0, 0);
      block.userData.isBreadcrumb = true;
      block.userData.crumbIndex = i;

      block.add(new ThreeMeshUI.Text({
        content: labels[i]!,
        fontSize: 0.011,
        fontColor: fontColor(labelColor),
      }));

      this.group.add(block);
      this.crumbs.push({ block, label: labels[i]! });

      x += crumbW / 2;

      // Separator (own Block since MSDF text needs one) after all but last.
      if (i < labels.length - 1) {
        x += gap;
        const sepBlock = new ThreeMeshUI.Block({
          ...FONT_BLOCK_OPTS,
          width: sepW, height: crumbH,
          backgroundOpacity: 0, borderOpacity: 0,
          justifyContent: 'center', alignItems: 'center',
        });
        sepBlock.position.set(x + sepW / 2, 0, 0.001);
        sepBlock.add(new ThreeMeshUI.Text({
          content: '>',
          fontSize: 0.009,
          fontColor: fontColor(TEXT.dim),
        }));
        this.group.add(sepBlock);
        this.separators.push(sepBlock);
        x += sepW + gap;
        x += crumbW / 2;
      }
    }
  }

  /** Get the clickable blocks for Interact registration */
  getBlocks(): { block: InstanceType<typeof ThreeMeshUI.Block>; index: number }[] {
    return this.crumbs.map((c, i) => ({ block: c.block, index: i }));
  }

  /** Get the onNavigate callback */
  navigate(depthIndex: number): void {
    this.onNavigate(depthIndex);
  }
}
