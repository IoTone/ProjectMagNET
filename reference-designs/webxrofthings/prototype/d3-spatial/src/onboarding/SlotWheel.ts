/**
 * SlotWheel — per-slot character selector for the join-code panel.
 *
 * Shows a vertical "fruit machine" selector above the active code slot:
 *   up arrow | current char (large) | down arrow
 *
 * Cycles through CHAR_SET (A-Z, 2-9, 30 chars, no 0/O/1/I/L).
 */

import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { CHAR_SET } from './types';
import { TEXT } from '../ui/palette';

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

  // Current character display (troika text for large char)
  const charDisplay = new Text();
  charDisplay.text = CHAR_SET[0]!;
  charDisplay.fontSize = 0.028;
  charDisplay.color = TEXT.emphasis;
  charDisplay.anchorX = 'center';
  charDisplay.anchorY = 'middle';
  charDisplay.position.set(0, 0, 0.002);
  charDisplay.sync();
  g.add(charDisplay);

  // Background block
  const bgBlock = new ThreeMeshUI.Block({
    width: 0.05,
    height: 0.09,
    backgroundOpacity: 0.92,
    backgroundColor: new THREE.Color(0x2a2520),
    borderRadius: 0.006,
    borderWidth: 0.001,
    borderColor: new THREE.Color(0xb8a380),
    borderOpacity: 0.8,
  } as any);
  bgBlock.position.set(0, 0, -0.001);
  g.add(bgBlock);

  // Up arrow button
  const upBlock = new ThreeMeshUI.Block({
    width: 0.04,
    height: 0.02,
    backgroundOpacity: 0.85,
    backgroundColor: new THREE.Color(0x3a3530),
    borderRadius: 0.003,
    justifyContent: 'center',
    alignItems: 'center',
  } as any);
  upBlock.position.set(0, 0.032, 0.002);
  g.add(upBlock);

  const upLabel = new Text();
  upLabel.text = '\u25b2'; // up triangle
  upLabel.fontSize = 0.012;
  upLabel.color = TEXT.primary;
  upLabel.anchorX = 'center';
  upLabel.anchorY = 'middle';
  upLabel.position.set(0, 0.032, 0.004);
  upLabel.sync();
  g.add(upLabel);

  // Down arrow button
  const downBlock = new ThreeMeshUI.Block({
    width: 0.04,
    height: 0.02,
    backgroundOpacity: 0.85,
    backgroundColor: new THREE.Color(0x3a3530),
    borderRadius: 0.003,
    justifyContent: 'center',
    alignItems: 'center',
  } as any);
  downBlock.position.set(0, -0.032, 0.002);
  g.add(downBlock);

  const downLabel = new Text();
  downLabel.text = '\u25bc'; // down triangle
  downLabel.fontSize = 0.012;
  downLabel.color = TEXT.primary;
  downLabel.anchorX = 'center';
  downLabel.anchorY = 'middle';
  downLabel.position.set(0, -0.032, 0.004);
  downLabel.sync();
  g.add(downLabel);

  function updateDisplay() {
    charDisplay.text = CHAR_SET[charIndex]!;
    charDisplay.sync();
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
