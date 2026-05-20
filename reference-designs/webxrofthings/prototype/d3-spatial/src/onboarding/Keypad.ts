/**
 * Keypad — stacked A–Z + 0–9 popup for the join-code panel.
 *
 * Replaces the per-slot up/down SlotWheel. The wheel forced six rounds of
 * scroll-up-or-down per character; in spatial input that's brutal. A
 * stacked keypad lets the user tap the exact character they want.
 *
 * Layout (keys are ThreeMeshUI Blocks so they get the same hit-testing
 * the rest of the join panel uses):
 *
 *     A B C D E F G H I J K L M
 *     N O P Q R S T U V W X Y Z
 *           0 1 2 3 4 5 6 7 8 9
 *
 *     [⌫ Back]            [✓ Done]
 *
 * The Keypad doesn't track which slot is active — that's JoinPanel's job.
 * The Keypad just publishes a list of pressable keys (`getKeys()`); the
 * panel registers them with Interact and routes the resulting `onSelect`s
 * into setSlotChar / backspace / dismiss as appropriate.
 *
 * Sized so 13 keys + their gaps fit comfortably inside the 0.4-wide join
 * panel. Positioned by the caller — typically just below the slots row
 * so the popup reads as "below the input field" like a phone keyboard.
 */

import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';

/** One pressable element on the keypad. `kind` lets JoinPanel switch on
 *  behaviour without parsing the visible character. */
export interface KeypadKey {
  /** The character to insert ('A'..'Z' or '0'..'9') or 'BACK' / 'DONE'. */
  char: string;
  kind: 'char' | 'backspace' | 'done';
  /** ThreeMeshUI Block — opaque to the keypad, registered by JoinPanel
   *  with the Interact system for hover + select. */
  block: any;
}

export interface KeypadResult {
  group: THREE.Group;
  getKeys(): KeypadKey[];
  /** Toggle the hover highlight on a key block (call from Interact's
   *  onHoverIn/onHoverOut). Bright cyan fill + border so the focused key
   *  is unmistakable on a low-contrast XR display. */
  setHover(block: any, on: boolean): void;
  /** Brief brighter flash on select, then revert to hover-or-default.
   *  Pairs with the click sound for tactile-feeling feedback. */
  pressFlash(block: any): void;
  show(): void;
  hide(): void;
  visible(): boolean;
  dispose(): void;
}

export function createKeypad(): KeypadResult {
  const g = new THREE.Group();
  g.name = 'keypad';
  g.visible = false;
  /* +10% over the original 0.022-key sizing so it reads bigger on the
   * dataspace-login screen without re-tuning every constant below.
   * Uniform scale also expands hit targets — good for XR raycast dwell. */
  g.scale.setScalar(1.1);

  // ─── Geometry constants ────────────────────────────────────────────
  //
  // Char-key size + gap chosen so 13 keys (the widest row) + 12 gaps
  // come to 0.286 world units — comfortably narrower than the 0.4-wide
  // join panel above it. Stays large enough to hit-target in XR
  // (Quest controller dwell-select needs ~2 cm; 0.022 ≈ 2.2 cm).
  const KEY_SIZE = 0.022;
  const KEY_GAP = 0.003;
  const ROW_GAP = KEY_SIZE + KEY_GAP;

  const ROW_TOP    = 'ABCDEFGHIJKLM';
  const ROW_MIDDLE = 'NOPQRSTUVWXYZ';
  const ROW_DIGITS = '0123456789';

  const keys: KeypadKey[] = [];
  /** Labels we own — disposed in `dispose()` to free troika atlas slots. */
  const labels: Text[] = [];

  // Highlight palette. Hover = bright cyan (matches the boombox /
  // globe accent so the whole app's "focused/active" colour is
  // consistent); flash = near-white pop on select that decays back.
  const HOVER_BG = new THREE.Color(0x2e5a78);
  const HOVER_BORDER = new THREE.Color(0x7fd1ff);
  const FLASH_BG = new THREE.Color(0x9fe4ff);
  let hoveredBlock: any = null;

  /** Stash a block's resting style on its userData so setHover can revert
   *  exactly (char keys and the two action keys have different palettes). */
  function rememberStyle(block: any, bg: number, border: number, borderOp: number, bgOp: number) {
    block.userData.defBg = new THREE.Color(bg);
    block.userData.defBorder = new THREE.Color(border);
    block.userData.defBorderOp = borderOp;
    block.userData.defBgOp = bgOp;
  }
  function applyDefault(block: any) {
    block.set({
      backgroundColor: block.userData.defBg,
      borderColor: block.userData.defBorder,
      borderOpacity: block.userData.defBorderOp,
      backgroundOpacity: block.userData.defBgOp,
    });
  }
  function applyHover(block: any) {
    block.set({
      backgroundColor: HOVER_BG,
      borderColor: HOVER_BORDER,
      borderOpacity: 1.0,
      backgroundOpacity: 1.0,
    });
  }

  // ─── Background panel ──────────────────────────────────────────────
  //
  // Slightly wider/taller than the key grid so each key sits in a clear
  // padded "tray." The same colour palette as the slot blocks in the
  // join panel keeps the popup visually anchored to the parent.
  const bgWidth  = ROW_TOP.length * KEY_SIZE + (ROW_TOP.length - 1) * KEY_GAP + 0.02;
  const bgHeight = 4 * KEY_SIZE + 3 * KEY_GAP + 0.025;
  const bgBlock = new ThreeMeshUI.Block({
    width: bgWidth,
    height: bgHeight,
    backgroundOpacity: 0.95,
    backgroundColor: new THREE.Color(0x1a1815),
    borderRadius: 0.006,
    borderWidth: 0.001,
    borderColor: new THREE.Color(0xb8a380),
    borderOpacity: 0.85,
  } as any);
  bgBlock.position.set(0, 0, -0.0015);
  g.add(bgBlock);

  /** Build one row of single-character keys at vertical position `y`. */
  function buildCharRow(chars: string, y: number) {
    const rowWidth = chars.length * KEY_SIZE + (chars.length - 1) * KEY_GAP;
    const startX = -rowWidth / 2 + KEY_SIZE / 2;
    for (let i = 0; i < chars.length; i++) {
      const ch = chars[i]!;
      const x = startX + i * (KEY_SIZE + KEY_GAP);

      const block = new ThreeMeshUI.Block({
        width: KEY_SIZE,
        height: KEY_SIZE,
        backgroundOpacity: 0.92,
        backgroundColor: new THREE.Color(0x3a3530),
        borderRadius: 0.003,
        borderWidth: 0.0006,
        borderColor: new THREE.Color(TEXT.muted),
        borderOpacity: 0.55,
        justifyContent: 'center',
        alignItems: 'center',
      } as any);
      block.position.set(x, y, 0.001);
      rememberStyle(block, 0x3a3530, TEXT.muted, 0.55, 0.92);
      g.add(block);

      const label = new Text();
      label.text = ch;
      label.fontSize = 0.013;
      label.color = TEXT.primary;
      label.anchorX = 'center';
      label.anchorY = 'middle';
      label.position.set(x, y, 0.004);
      label.sync();
      g.add(label);
      labels.push(label);

      keys.push({ char: ch, kind: 'char', block });
    }
  }

  // Row positions: top → bottom inside the tray.
  //   ROW_TOP at +1.5×rowGap, MIDDLE at +0.5×, DIGITS at -0.5×,
  //   action row at -1.5×rowGap.
  buildCharRow(ROW_TOP,    ROW_GAP * 1.5);
  buildCharRow(ROW_MIDDLE, ROW_GAP * 0.5);
  buildCharRow(ROW_DIGITS, -ROW_GAP * 0.5);

  // ─── Action row: Backspace + Done ──────────────────────────────────
  const actionY = -ROW_GAP * 1.5;

  function buildActionKey(opts: {
    label: string;
    x: number;
    width: number;
    bg: number;
    color: number;
    char: string;
    kind: 'backspace' | 'done';
  }) {
    const block = new ThreeMeshUI.Block({
      width: opts.width,
      height: KEY_SIZE,
      backgroundOpacity: 0.94,
      backgroundColor: new THREE.Color(opts.bg),
      borderRadius: 0.004,
      borderWidth: 0.0006,
      borderColor: new THREE.Color(opts.color),
      borderOpacity: 0.6,
      justifyContent: 'center',
      alignItems: 'center',
    } as any);
    block.position.set(opts.x, actionY, 0.001);
    rememberStyle(block, opts.bg, opts.color, 0.6, 0.94);
    g.add(block);

    const text = new Text();
    text.text = opts.label;
    text.fontSize = 0.011;
    text.color = opts.color;
    text.anchorX = 'center';
    text.anchorY = 'middle';
    text.position.set(opts.x, actionY, 0.004);
    text.sync();
    g.add(text);
    labels.push(text);

    keys.push({ char: opts.char, kind: opts.kind, block });
  }

  // Backspace clears the current slot (or steps back if empty); coloured
  // warm-red to read as a destructive-ish action.
  buildActionKey({
    label: '⌫ Back',
    x: -0.07,
    width: 0.07,
    bg: 0x4a2520,
    color: 0xe6a070,
    char: 'BACK',
    kind: 'backspace',
  });

  // Done explicitly dismisses the keypad without submitting — useful when
  // the user has filled some slots but wants to inspect before pressing
  // the main Submit. Coloured green to match the Submit button.
  buildActionKey({
    label: '✓ Done',
    x:  0.07,
    width: 0.07,
    bg: 0x2a4a2a,
    color: 0xc0e0a0,
    char: 'DONE',
    kind: 'done',
  });

  return {
    group: g,
    getKeys: () => keys,
    setHover: (block: any, on: boolean) => {
      if (on) { hoveredBlock = block; applyHover(block); }
      else { if (hoveredBlock === block) hoveredBlock = null; applyDefault(block); }
    },
    pressFlash: (block: any) => {
      block.set({
        backgroundColor: FLASH_BG,
        borderColor: FLASH_BG,
        borderOpacity: 1.0,
        backgroundOpacity: 1.0,
      });
      // Revert after a short beat. If the pointer is still on this key,
      // settle back to the hover style rather than fully default so the
      // focus state stays coherent.
      setTimeout(() => {
        if (hoveredBlock === block) applyHover(block);
        else applyDefault(block);
      }, 130);
    },
    show: () => { g.visible = true; },
    hide: () => {
      g.visible = false;
      // Clear any lingering hover style so the keypad doesn't reappear
      // with a stale highlighted key next time it's shown.
      if (hoveredBlock) { applyDefault(hoveredBlock); hoveredBlock = null; }
    },
    visible: () => g.visible,
    dispose: () => {
      for (const lbl of labels) lbl.dispose();
      labels.length = 0;
    },
  };
}
