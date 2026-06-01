import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';

export type ScaleTag = 'personal' | 'room' | 'hall' | 'net';

export interface Dataspace {
  id: string;
  name: string;
  scaleTag: ScaleTag;
  color: number;
  glyph: string;
}

export const SCALE_RADIUS: Record<ScaleTag, number> = {
  personal: 0.3,
  room: 1.5,
  hall: 4,
  net: 8,
};

export class DataspaceRegistry {
  private spaces = new Map<string, Dataspace>();
  private focused: string | null = null;
  private listeners: Array<(id: string | null) => void> = [];

  add(ds: Dataspace) { this.spaces.set(ds.id, ds); }
  list(): Dataspace[] { return [...this.spaces.values()]; }
  get(id: string): Dataspace | undefined { return this.spaces.get(id); }
  focus(id: string | null) {
    if (id === this.focused) return;
    this.focused = id;
    for (const fn of this.listeners) fn(id);
  }
  getFocused(): string | null { return this.focused; }
  onFocusChange(fn: (id: string | null) => void) { this.listeners.push(fn); }
}

/** Thin horizontal strip of dataspace chips, placed at top of uiAnchor. */
export class DataspaceHud {
  readonly group = new THREE.Group();
  private chips: Array<{ id: string; block: any; text: Text }> = [];

  constructor(private registry: DataspaceRegistry) {
    this.group.name = 'dataspace-hud';
    this.rebuild();
    registry.onFocusChange(() => this.refresh());
  }

  rebuild() {
    this.group.clear();
    this.chips = [];
    const spaces = this.registry.list();
    const CHIP_W = 0.12;
    const CHIP_H = 0.03;
    const GAP = 0.012;
    const total = spaces.length * CHIP_W + (spaces.length - 1) * GAP;
    let x = -total / 2 + CHIP_W / 2;

    for (const ds of spaces) {
      const chip = new ThreeMeshUI.Block({
        width: CHIP_W,
        height: CHIP_H,
        padding: 0.004,
        borderRadius: 0.006,
        backgroundColor: new THREE.Color(0x0f1a2c),
        backgroundOpacity: 0.85,
        borderWidth: 0.0008,
        borderColor: new THREE.Color(ds.color),
        borderOpacity: 0.7,
      });
      chip.position.set(x, 0, 0);
      x += CHIP_W + GAP;

      const text = new Text();
      text.text = `${ds.glyph}  ${ds.name}`;
      text.fontSize = 0.011;
      text.color = ds.color;
      text.anchorX = 'center';
      text.anchorY = 'middle';
      text.position.set(0, 0, 0.002);
      text.sync();
      chip.add(text);

      this.group.add(chip);
      this.chips.push({ id: ds.id, block: chip, text });
    }
    this.refresh();
  }

  private refresh() {
    const focused = this.registry.getFocused();
    for (const c of this.chips) {
      const ds = this.registry.get(c.id)!;
      const active = focused === null || focused === c.id;
      c.block.set({
        borderOpacity: active ? 1 : 0.25,
        backgroundOpacity: active ? 0.95 : 0.5,
      });
      c.text.color = active ? ds.color : TEXT.dim;
      c.text.sync();
    }
  }
}

/** Apply focus as an opacity/scale dim on a mark's Object3D. */
export function applyFocusDim(
  group: THREE.Object3D,
  markDataspaceId: string,
  focused: string | null,
) {
  const active = focused === null || focused === markDataspaceId;
  group.scale.setScalar(active ? 1 : 0.7);
  group.traverse(o => {
    const m = o as THREE.Mesh;
    const mat = m.material as THREE.MeshStandardMaterial | undefined;
    if (!mat) return;
    if ('opacity' in mat) {
      (mat as any).transparent = true;
      (mat as any).opacity = active ? 1.0 : 0.08;
    }
    if ('emissiveIntensity' in mat) {
      if ((mat as any).__focusOrigEmissive === undefined) {
        (mat as any).__focusOrigEmissive = mat.emissiveIntensity;
      }
      mat.emissiveIntensity = active ? (mat as any).__focusOrigEmissive : 0;
    }
  });
}
