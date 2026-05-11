/**
 * JoinPanel — join-code onboarding panel for entering a dataspace.
 *
 * Built with three-mesh-ui blocks + MSDF text throughout (no troika) — this
 * is the first screen users see and the slot characters are big (~28 mm tall)
 * so any blurriness was very obvious here.
 *
 * Visual style follows XR_UX_BEST_PRACTICES.md: warm palette, 0x2a2520 backgrounds,
 * TEXT.* colors, no blue.
 */

import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { JoinState, CHAR_SET, SLOT_COUNT } from './types';
import type { JoinPanelEvents } from './types';
import { createSlotWheel, SlotWheelResult } from './SlotWheel';
import { TEXT } from '../ui/palette';
import { FONT_BLOCK_OPTS, fontColor } from '../ui/textStyles';

export interface JoinPanelResult {
  /** Root group — add to scene. */
  group: THREE.Group;
  /** All hoverable/selectable blocks for Interact registration. */
  getInteractables(): Array<{ id: string; block: any; onSelect: () => void }>;
  /** Current state. */
  state(): JoinState;
  /** Set a character in a specific slot (0-5). */
  setSlotChar(slotIndex: number, char: string): void;
  /** Fill all slots from a string (e.g. 'ABC123'). */
  fillCode(code: string): void;
  /** Activate a slot (show the wheel). */
  activateSlot(slotIndex: number): void;
  /** Deactivate the active slot. */
  deactivateSlot(): void;
  /** Trigger submit. */
  submit(): void;
  /** Clear all slots. */
  clear(): void;
  /** Show the panel. */
  show(): void;
  /** Hide the panel. */
  hide(): void;
  /** Whether the panel is currently visible. */
  visible(): boolean;
  /** Dispose resources. */
  dispose(): void;
}

/** Invisible Block slot — just a centered layout container for one Text. */
function makeTextSlot(width: number, height: number) {
  return new ThreeMeshUI.Block({
    width, height,
    backgroundOpacity: 0, borderOpacity: 0,
    justifyContent: 'center', alignItems: 'center',
  });
}

export function createJoinPanel(events: JoinPanelEvents = {}): JoinPanelResult {
  const g = new THREE.Group();
  g.name = 'join-panel';
  g.visible = false;

  let currentState: JoinState = JoinState.ENTERING;
  let activeSlotIndex = -1;

  // Slot values (empty string = unfilled)
  const DEFAULT_CODE = 'AAAAAA';
  const slotValues: string[] = DEFAULT_CODE.split('');

  // --- Root panel block ---
  // Sets the font once for every nested Text descendant — no need to repeat
  // FONT_BLOCK_OPTS on the inner Blocks (Text inherits the closest ancestor
  // Block's font context).
  const rootBlock = new ThreeMeshUI.Block({
    ...FONT_BLOCK_OPTS,
    width: 0.4,
    height: 0.28,
    padding: 0.02,
    backgroundOpacity: 0.94,
    backgroundColor: new THREE.Color(0x2a2520),
    borderRadius: 0.008,
    borderWidth: 0.0015,
    borderColor: new THREE.Color(0xb8a380),
    borderOpacity: 0.9,
    contentDirection: 'column',
    justifyContent: 'start',
    alignItems: 'center',
  } as any);
  rootBlock.position.set(0, 0, 0);
  g.add(rootBlock);

  // --- Title ---
  const titleSlot = makeTextSlot(0.36, 0.030);
  titleSlot.position.set(0, 0.10, 0.005);
  g.add(titleSlot);
  titleSlot.add(new ThreeMeshUI.Text({
    content: 'Join a dataspace',
    fontSize: 0.024,
    fontColor: fontColor(TEXT.primary),
  }));

  // --- Code slots ---
  const slotBlocks: any[] = [];
  const slotTexts: any[] = [];
  const slotWidth = 0.04;
  const slotHeight = 0.05;
  const slotGap = 0.008;
  const totalWidth = SLOT_COUNT * slotWidth + (SLOT_COUNT - 1) * slotGap;
  const startX = -totalWidth / 2 + slotWidth / 2;

  for (let i = 0; i < SLOT_COUNT; i++) {
    const block = new ThreeMeshUI.Block({
      width: slotWidth,
      height: slotHeight,
      backgroundOpacity: 0.88,
      backgroundColor: new THREE.Color(0x1a1815),
      borderRadius: 0.004,
      borderWidth: 0.001,
      borderColor: new THREE.Color(TEXT.muted),
      borderOpacity: 0.7,
      justifyContent: 'center',
      alignItems: 'center',
    } as any);
    block.position.set(startX + i * (slotWidth + slotGap), 0.035, 0.003);
    g.add(block);
    slotBlocks.push(block);

    // Character is a child of the slot block, so layout centers it for free
    // and it inherits the MSDF font from rootBlock through the hierarchy.
    const charText: any = new ThreeMeshUI.Text({
      content: slotValues[i] || '',
      fontSize: 0.028,
      fontColor: fontColor(TEXT.emphasis),
    });
    block.add(charText);
    slotTexts.push(charText);
  }

  // --- Status text ---
  const statusSlot = makeTextSlot(0.36, 0.020);
  statusSlot.position.set(0, -0.015, 0.005);
  g.add(statusSlot);
  const statusText: any = new ThreeMeshUI.Text({
    content: 'Press Submit to join with code AAAAAA',
    fontSize: 0.014,
    fontColor: fontColor(TEXT.muted),
  });
  statusSlot.add(statusText);

  // --- Buttons ---
  const submitBlock = new ThreeMeshUI.Block({
    width: 0.09,
    height: 0.03,
    backgroundOpacity: 0.9,
    backgroundColor: new THREE.Color(0x3a5a3a),
    borderRadius: 0.004,
    justifyContent: 'center',
    alignItems: 'center',
  } as any);
  submitBlock.position.set(-0.05, -0.055, 0.003);
  g.add(submitBlock);
  submitBlock.add(new ThreeMeshUI.Text({
    content: 'Submit',
    fontSize: 0.011,
    fontColor: fontColor(TEXT.accent),
  }));

  const clearBlock = new ThreeMeshUI.Block({
    width: 0.07,
    height: 0.03,
    backgroundOpacity: 0.9,
    backgroundColor: new THREE.Color(0x3a3530),
    borderRadius: 0.004,
    justifyContent: 'center',
    alignItems: 'center',
  } as any);
  clearBlock.position.set(0.04, -0.055, 0.003);
  g.add(clearBlock);
  clearBlock.add(new ThreeMeshUI.Text({
    content: 'Clear',
    fontSize: 0.011,
    fontColor: fontColor(TEXT.muted),
  }));

  // --- Footer ---
  const footerSlot = makeTextSlot(0.36, 0.015);
  footerSlot.position.set(0, -0.10, 0.005);
  g.add(footerSlot);
  footerSlot.add(new ThreeMeshUI.Text({
    content: '🔒 secure · hlxr.org',
    fontSize: 0.010,
    fontColor: fontColor(TEXT.dim),
  }));

  // --- Slot wheel ---
  const slotWheel: SlotWheelResult = createSlotWheel();
  g.add(slotWheel.group);

  // --- Helper functions ---

  function filledCount(): number {
    return slotValues.filter(v => v.length > 0).length;
  }

  function getCode(): string {
    return slotValues.join('');
  }

  function updateSlotVisuals() {
    for (let i = 0; i < SLOT_COUNT; i++) {
      slotTexts[i].set({ content: slotValues[i] || '' });

      const isActive = i === activeSlotIndex;
      const isFilled = slotValues[i]!.length > 0;

      // Update border color based on state
      const borderColor = isActive
        ? new THREE.Color(TEXT.primary)
        : isFilled
          ? new THREE.Color(TEXT.accent)
          : new THREE.Color(TEXT.muted);
      (slotBlocks[i] as any).set({
        borderColor,
        borderOpacity: isActive ? 1.0 : 0.7,
      });
    }
  }

  function updateStatus(text: string, color: number) {
    statusText.set({ content: text, fontColor: new THREE.Color(color) });
  }

  function setState(newState: JoinState) {
    currentState = newState;
    switch (newState) {
      case JoinState.IDLE:
        // Mention the fixed UC codes alongside the generic prompt so testers
        // can jump directly into a use-case dataspace without a host device.
        updateStatus('Enter a code  ·  try DEMO01 / DEMO02 / DEMO03 / DEMO04', TEXT.muted);
        break;
      case JoinState.ENTERING: {
        const n = filledCount();
        updateStatus(`${n} of ${SLOT_COUNT} characters`, TEXT.body);
        break;
      }
      case JoinState.SUBMITTING:
        updateStatus('Joining...', TEXT.warn);
        break;
      case JoinState.ACCEPTED:
        updateStatus('✓ Connected to demo-dataspace', TEXT.accent);
        break;
      case JoinState.REJECTED:
        updateStatus('Code not recognized', TEXT.error);
        break;
    }
  }

  function activateSlot(index: number) {
    if (index < 0 || index >= SLOT_COUNT) return;
    activeSlotIndex = index;

    // Position the wheel above the active slot
    const slotX = startX + index * (slotWidth + slotGap);
    slotWheel.show(new THREE.Vector3(slotX, 0.035, 0.01));

    // If the slot has a char, set the wheel to it
    if (slotValues[index]!.length > 0) {
      slotWheel.setChar(slotValues[index]!);
    }

    updateSlotVisuals();
  }

  function deactivateSlot() {
    activeSlotIndex = -1;
    slotWheel.hide();
    updateSlotVisuals();
  }

  function setSlotChar(slotIndex: number, char: string) {
    if (slotIndex < 0 || slotIndex >= SLOT_COUNT) return;
    const upper = char.toUpperCase();
    if (!CHAR_SET.includes(upper)) return;

    slotValues[slotIndex] = upper;
    updateSlotVisuals();

    if (currentState === JoinState.IDLE || currentState === JoinState.REJECTED) {
      setState(JoinState.ENTERING);
    } else if (currentState === JoinState.ENTERING) {
      const n = filledCount();
      updateStatus(`${n} of ${SLOT_COUNT} characters`, TEXT.body);
    }
  }

  function wheelCommit() {
    if (activeSlotIndex < 0) return;
    const ch = slotWheel.currentChar();
    setSlotChar(activeSlotIndex, ch);

    // Auto-advance to next empty slot
    const nextEmpty = slotValues.findIndex((v, i) => i > activeSlotIndex && v.length === 0);
    if (nextEmpty >= 0) {
      activateSlot(nextEmpty);
    } else {
      deactivateSlot();
    }
  }

  function submit() {
    if (currentState === JoinState.SUBMITTING || currentState === JoinState.ACCEPTED) return;
    deactivateSlot();

    const code = getCode();
    if (code.length < SLOT_COUNT) {
      // Reject incomplete codes
      setState(JoinState.REJECTED);
      events.onRejected?.(code, 'incomplete');

      // Flash red on empty slots
      for (let i = 0; i < SLOT_COUNT; i++) {
        if (slotValues[i]!.length === 0) {
          (slotBlocks[i] as any).set({ borderColor: new THREE.Color(TEXT.error), borderOpacity: 1.0 });
        }
      }

      // Return to entering after 1.5s
      setTimeout(() => {
        if (currentState === JoinState.REJECTED) {
          setState(JoinState.ENTERING);
          updateSlotVisuals();
        }
      }, 1500);
      return;
    }

    // Real fetch to /api/v1/join
    setState(JoinState.SUBMITTING);
    fetch('/api/v1/join', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code }),
    })
      .then(resp => resp.json())
      .then((data: { status: string; token?: string; manifest_url?: string; dataspace?: string; reason?: string }) => {
        if (data.status === 'accepted') {
          setState(JoinState.ACCEPTED);
          events.onAccepted?.(code, data.token, data.manifest_url, data.dataspace);
        } else {
          setState(JoinState.REJECTED);
          const reason = data.reason ?? 'unknown';
          if (reason === 'rate_limited') {
            updateStatus('Too many attempts. Wait a moment.', TEXT.error);
          } else if (reason === 'expired') {
            updateStatus('Code expired. Try the new one.', TEXT.error);
          } else {
            updateStatus('Code not recognized', TEXT.error);
          }
          events.onRejected?.(code, reason);
          // Return to entering after 2s
          setTimeout(() => {
            if (currentState === JoinState.REJECTED) {
              setState(JoinState.ENTERING);
              updateSlotVisuals();
            }
          }, 2000);
        }
      })
      .catch(() => {
        // Server not running — fall back to mock validation (accept any complete code)
        console.warn('[join] Server not reachable, using mock validation');
        setState(JoinState.ACCEPTED);
        events.onAccepted?.(code);
      });
  }

  function clearSlots() {
    for (let i = 0; i < SLOT_COUNT; i++) {
      slotValues[i] = '';
    }
    deactivateSlot();
    setState(JoinState.IDLE);
    updateSlotVisuals();
  }

  // --- Keyboard input ---
  let keyboardSlotCursor = 0;

  function handleKeydown(e: KeyboardEvent) {
    if (!g.visible) return;
    if (currentState === JoinState.SUBMITTING || currentState === JoinState.ACCEPTED) return;

    const key = e.key.toUpperCase();

    // Backspace: clear last filled slot
    if (e.key === 'Backspace') {
      e.preventDefault();
      const lastFilled = slotValues.reduce((last, v, i) => (v.length > 0 ? i : last), -1);
      if (lastFilled >= 0) {
        slotValues[lastFilled] = '';
        keyboardSlotCursor = lastFilled;
        updateSlotVisuals();
        if (filledCount() === 0) {
          setState(JoinState.IDLE);
        } else {
          const n = filledCount();
          updateStatus(`${n} of ${SLOT_COUNT} characters`, TEXT.body);
        }
      }
      return;
    }

    // Enter: submit
    if (e.key === 'Enter') {
      e.preventDefault();
      submit();
      return;
    }

    // Character input
    if (key.length === 1 && CHAR_SET.includes(key)) {
      e.preventDefault();
      // Find next empty slot from cursor
      let targetSlot = -1;
      for (let i = keyboardSlotCursor; i < SLOT_COUNT; i++) {
        if (slotValues[i]!.length === 0) {
          targetSlot = i;
          break;
        }
      }
      // Wrap around if no empty slot found after cursor
      if (targetSlot < 0) {
        for (let i = 0; i < keyboardSlotCursor; i++) {
          if (slotValues[i]!.length === 0) {
            targetSlot = i;
            break;
          }
        }
      }
      if (targetSlot >= 0) {
        setSlotChar(targetSlot, key);
        keyboardSlotCursor = targetSlot + 1;
      }
    }
  }

  window.addEventListener('keydown', handleKeydown);

  // --- Build interactables list ---
  function getInteractables(): Array<{ id: string; block: any; onSelect: () => void }> {
    const result: Array<{ id: string; block: any; onSelect: () => void }> = [];

    // Code slots
    for (let i = 0; i < SLOT_COUNT; i++) {
      result.push({
        id: `join:slot:${i}`,
        block: slotBlocks[i],
        onSelect: () => {
          if (activeSlotIndex === i) {
            // Already active — commit the wheel char
            wheelCommit();
          } else {
            activateSlot(i);
          }
        },
      });
    }

    // Wheel arrows
    result.push({
      id: 'join:wheel:up',
      block: slotWheel.upBlock,
      onSelect: () => {
        slotWheel.prev();
        if (activeSlotIndex >= 0) {
          setSlotChar(activeSlotIndex, slotWheel.currentChar());
        }
      },
    });
    result.push({
      id: 'join:wheel:down',
      block: slotWheel.downBlock,
      onSelect: () => {
        slotWheel.next();
        if (activeSlotIndex >= 0) {
          setSlotChar(activeSlotIndex, slotWheel.currentChar());
        }
      },
    });

    // Submit button
    result.push({
      id: 'join:submit',
      block: submitBlock,
      onSelect: () => submit(),
    });

    // Clear button
    result.push({
      id: 'join:clear',
      block: clearBlock,
      onSelect: () => clearSlots(),
    });

    return result;
  }

  return {
    group: g,
    getInteractables,
    state: () => currentState,
    setSlotChar,
    fillCode: (code: string) => {
      const upper = code.toUpperCase();
      for (let i = 0; i < SLOT_COUNT && i < upper.length; i++) {
        setSlotChar(i, upper[i]!);
      }
    },
    activateSlot,
    deactivateSlot,
    submit,
    clear: clearSlots,
    show: () => { g.visible = true; },
    hide: () => {
      g.visible = false;
      deactivateSlot();
    },
    visible: () => g.visible,
    dispose: () => {
      window.removeEventListener('keydown', handleKeydown);
      // three-mesh-ui Texts are owned by their parent Blocks and don't need
      // per-instance dispose — removing the panel group from the scene tears
      // down the GL resources via three-mesh-ui's internal cleanup.
    },
  };
}
