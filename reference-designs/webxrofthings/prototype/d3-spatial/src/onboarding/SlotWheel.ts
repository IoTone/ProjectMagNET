/**
 * SlotWheel — per-slot character selector for the join-code panel.
 *
 * Shows a vertical "fruit machine" selector above the active code slot:
 *   up arrow | current char (large) | down arrow
 *
 * Cycles through CHAR_SET (A-Z, 2-9, 30 chars, no 0/O/1/I/L).
 *
 * MSDF text via three-mesh-ui throughout — the big character glyph is the
 * largest text in the join flow, so blurriness was most obvious here.
 */

import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { CHAR_SET } from './types';
import { TEXT } from '../ui/palette';
import { FONT_BLOCK_OPTS, fontColor } from '../ui/textStyles';

export interface SlotWheelResult {
  group: THREE.Group;
  /** The three-mesh-ui block for the up arrow (register with Interact). */
  upBlock: any;
  /** The three-mesh-ui block for the down arrow (register with Interact). */
  downBlock: any;
  /** Get the currently selected character. */
  currentChar(): string;
  /** Set the character (e.g. from keyboard input). */
  setChar(ch: string): void;
  /** Cycle to the next character in the set. */
  next(): void;
  /** Cycle to the previous character in the set. */
  prev(): void;
  /** Show the wheel at a world position. */
  show(worldPos: THREE.Vector3): void;
  /** Hide the wheel. */
  hide(): void;
  /** Whether the wheel is currently visible. */
  visible(): boolean;
}

export function createSlotWheel(): SlotWheelResult {
  const g = new THREE.Group();
  g.name = 'slot-wheel';
  g.visible = false;

  let charIndex = 0;

  // Background block — now also the layout parent for the big char glyph.
  const bgBlock = new ThreeMeshUI.Block({
    ...FONT_BLOCK_OPTS,
    width: 0.05,
    height: 0.09,
    backgroundOpacity: 0.92,
    backgroundColor: new THREE.Color(0x2a2520),
    borderRadius: 0.006,
    borderWidth: 0.001,
    borderColor: new THREE.Color(0xb8a380),
    borderOpacity: 0.8,
    justifyContent: 'center',
    alignItems: 'center',
  });
  bgBlock.position.set(0, 0, -0.001);
  g.add(bgBlock);

  // Big current-character glyph — child of bg so it picks up the MSDF font.
  // We keep a typed `any` reference so we can `.set({content})` on cycle.
  const charDisplay: any = new ThreeMeshUI.Text({
    content: CHAR_SET[0]!,
    fontSize: 0.028,
    fontColor: fontColor(TEXT.emphasis),
  });
  bgBlock.add(charDisplay);

  // Up arrow button — label is now a child of the button block instead of
  // a sibling in the group, so it inherits font context cleanly.
  const upBlock = new ThreeMeshUI.Block({
    ...FONT_BLOCK_OPTS,
    width: 0.04,
    height: 0.02,
    backgroundOpacity: 0.85,
    backgroundColor: new THREE.Color(0x3a3530),
    borderRadius: 0.003,
    justifyContent: 'center',
    alignItems: 'center',
  } as any);
  upBlock.position.set(0, 0.032, 0.002);
  upBlock.add(new ThreeMeshUI.Text({
    // ASCII fallback — the bundled Roboto-msdf doesn't include geometric
    // shape glyphs. A richer MSDF font (chars U+25B2/U+25BC) could restore
    // proper triangles later.
    content: '^',
    fontSize: 0.014,
    fontColor: fontColor(TEXT.primary),
  }));
  g.add(upBlock);

  // Down arrow button
  const downBlock = new ThreeMeshUI.Block({
    ...FONT_BLOCK_OPTS,
    width: 0.04,
    height: 0.02,
    backgroundOpacity: 0.85,
    backgroundColor: new THREE.Color(0x3a3530),
    borderRadius: 0.003,
    justifyContent: 'center',
    alignItems: 'center',
  } as any);
  downBlock.position.set(0, -0.032, 0.002);
  downBlock.add(new ThreeMeshUI.Text({
    content: 'v',                  // ASCII fallback for down triangle — see ^ note above
    fontSize: 0.014,
    fontColor: fontColor(TEXT.primary),
  }));
  g.add(downBlock);

  function updateDisplay() {
    charDisplay.set({ content: CHAR_SET[charIndex]! });
  }

  return {
    group: g,
    upBlock,
    downBlock,
    currentChar: () => CHAR_SET[charIndex]!,
    setChar: (ch: string) => {
      const upper = ch.toUpperCase();
      const idx = CHAR_SET.indexOf(upper);
      if (idx >= 0) {
        charIndex = idx;
        updateDisplay();
      }
    },
    next: () => {
      charIndex = (charIndex + 1) % CHAR_SET.length;
      updateDisplay();
    },
    prev: () => {
      charIndex = (charIndex - 1 + CHAR_SET.length) % CHAR_SET.length;
      updateDisplay();
    },
    show: (worldPos: THREE.Vector3) => {
      g.position.copy(worldPos);
      g.position.y += 0.06; // above the slot
      g.visible = true;
    },
    hide: () => {
      g.visible = false;
    },
    visible: () => g.visible,
  };
}
