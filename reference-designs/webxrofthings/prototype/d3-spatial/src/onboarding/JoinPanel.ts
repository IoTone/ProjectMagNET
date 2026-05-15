/**
 * JoinPanel — join-code onboarding panel for entering a dataspace.
 *
 * Phase 1: mock validation (accept any fully-filled 6-char code, reject incomplete).
 * Built with three-mesh-ui blocks + troika-three-text.
 *
 * Visual style follows XR_UX_BEST_PRACTICES.md: warm palette, 0x2a2520 backgrounds,
 * TEXT.* colors, no blue.
 */

import * as THREE from 'three';
import ThreeMeshUI from 'three-mesh-ui';
import { Text } from 'troika-three-text';
import { JoinState, CHAR_SET, SLOT_COUNT } from './types';
import type { JoinPanelEvents } from './types';
import { createKeypad, type KeypadResult } from './Keypad';
import { TEXT } from '../ui/palette';

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

export function createJoinPanel(events: JoinPanelEvents = {}): JoinPanelResult {
  const g = new THREE.Group();
  g.name = 'join-panel';
  g.visible = false;

  let currentState: JoinState = JoinState.ENTERING;
  let activeSlotIndex = -1;

  // Slot values (empty string = unfilled). Default pre-fills the first
  // demo dataspace (DEMO01) so a one-click Submit pulls up UC1 without
  // any wheel scrolling — speeds up demo runs and proves the panel's
  // wiring end-to-end before the user touches the keyboard. Other valid
  // codes today: DEMO02, DEMO03, DEMO04.
  const DEFAULT_CODE = 'DEMO01';
  const slotValues: string[] = DEFAULT_CODE.split('');

  // --- Root panel block ---
  const rootBlock = new ThreeMeshUI.Block({
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
  const titleText = new Text();
  titleText.text = 'Join a dataspace';
  titleText.fontSize = 0.024;
  titleText.color = TEXT.primary;
  titleText.anchorX = 'center';
  titleText.anchorY = 'middle';
  titleText.position.set(0, 0.10, 0.005);
  titleText.sync();
  g.add(titleText);

  // --- Code slots ---
  const slotBlocks: any[] = [];
  const slotTexts: Text[] = [];
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

    const charText = new Text();
    charText.text = slotValues[i] || '';
    charText.fontSize = 0.028;
    charText.color = TEXT.emphasis;
    charText.anchorX = 'center';
    charText.anchorY = 'middle';
    charText.position.set(startX + i * (slotWidth + slotGap), 0.035, 0.006);
    charText.sync();
    g.add(charText);
    slotTexts.push(charText);
  }

  // --- Status text ---
  const statusText = new Text();
  statusText.text = 'Press Submit to join with code DEMO01';
  statusText.fontSize = 0.014;
  statusText.color = TEXT.muted;
  statusText.anchorX = 'center';
  statusText.anchorY = 'middle';
  statusText.position.set(0, -0.015, 0.005);
  statusText.sync();
  g.add(statusText);

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

  const submitLabel = new Text();
  submitLabel.text = 'Submit';
  submitLabel.fontSize = 0.011;
  submitLabel.color = TEXT.accent;
  submitLabel.anchorX = 'center';
  submitLabel.anchorY = 'middle';
  submitLabel.position.set(-0.05, -0.055, 0.006);
  submitLabel.sync();
  g.add(submitLabel);

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

  const clearLabel = new Text();
  clearLabel.text = 'Clear';
  clearLabel.fontSize = 0.011;
  clearLabel.color = TEXT.muted;
  clearLabel.anchorX = 'center';
  clearLabel.anchorY = 'middle';
  clearLabel.position.set(0.04, -0.055, 0.006);
  clearLabel.sync();
  g.add(clearLabel);

  // --- Footer ---
  const footerText = new Text();
  footerText.text = '\ud83d\udd12 secure \u00b7 hlxr.org';
  footerText.fontSize = 0.010;
  footerText.color = TEXT.dim;
  footerText.anchorX = 'center';
  footerText.anchorY = 'middle';
  footerText.position.set(0, -0.10, 0.005);
  footerText.sync();
  g.add(footerText);

  // --- Keypad popup (stacked A-Z + 0-9) ---
  //
  // Replaces the per-slot up/down SlotWheel. Sits just below the join
  // panel and appears modally when a slot is active. Keys cycle the
  // active slot forward automatically; six taps fills the whole code.
  const keypad: KeypadResult = createKeypad();
  keypad.group.position.set(0, -0.235, 0);   // below the panel's lower edge
  g.add(keypad.group);

  // --- Helper functions ---

  function filledCount(): number {
    return slotValues.filter(v => v.length > 0).length;
  }

  function getCode(): string {
    return slotValues.join('');
  }

  function updateSlotVisuals() {
    for (let i = 0; i < SLOT_COUNT; i++) {
      slotTexts[i]!.text = slotValues[i] || '';
      slotTexts[i]!.sync();

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
    statusText.text = text;
    statusText.color = color;
    statusText.sync();
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
        updateStatus('\u2713 Connected to demo-dataspace', TEXT.accent);
        break;
      case JoinState.REJECTED:
        updateStatus('Code not recognized', TEXT.error);
        break;
    }
  }

  function activateSlot(index: number) {
    if (index < 0 || index >= SLOT_COUNT) return;
    activeSlotIndex = index;
    // Show the keypad. Stays open across slot-fills so the user can rip
    // through all six chars without re-tapping the panel between them.
    keypad.show();
    updateSlotVisuals();
  }

  function deactivateSlot() {
    activeSlotIndex = -1;
    keypad.hide();
    updateSlotVisuals();
  }

  /** Fill the active slot with `char`, advance the active cursor to the
   *  next slot. If we just filled the last slot, drop the cursor but
   *  keep the keypad open one extra moment so the user sees what they
   *  typed before the popup vanishes. */
  function typeChar(char: string) {
    if (activeSlotIndex < 0) return;
    setSlotChar(activeSlotIndex, char);
    if (activeSlotIndex < SLOT_COUNT - 1) {
      activateSlot(activeSlotIndex + 1);
    } else {
      // Filled the last slot — close the keypad so the user can see
      // their complete code and Submit it.
      deactivateSlot();
    }
  }

  /** Backspace: clear the current slot if it has a char, otherwise step
   *  the cursor back to the previous slot (which itself may have a char
   *  to clear on the next press). Mirrors phone-keyboard behaviour. */
  function backspace() {
    if (activeSlotIndex < 0) return;
    if (slotValues[activeSlotIndex]!.length > 0) {
      slotValues[activeSlotIndex] = '';
      updateSlotVisuals();
      const n = filledCount();
      if (n === 0) setState(JoinState.IDLE);
      else updateStatus(`${n} of ${SLOT_COUNT} characters`, TEXT.body);
    } else if (activeSlotIndex > 0) {
      activateSlot(activeSlotIndex - 1);
    }
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

    // Code slots — tap to activate the keypad on that slot. A second tap
    // on the same slot toggles the keypad closed (handy on Quest where
    // dismissing a popup is otherwise awkward).
    for (let i = 0; i < SLOT_COUNT; i++) {
      result.push({
        id: `join:slot:${i}`,
        block: slotBlocks[i],
        onSelect: () => {
          if (activeSlotIndex === i && keypad.visible()) {
            deactivateSlot();
          } else {
            activateSlot(i);
          }
        },
      });
    }

    // Keypad keys (A-Z, 0-9, Backspace, Done). Each registers as its own
    // interact target so a single hand-tracking pinch hits exactly the
    // intended key. JoinPanel routes each press into typeChar/backspace/
    // deactivateSlot based on the key's `kind`.
    for (const key of keypad.getKeys()) {
      result.push({
        id: `join:key:${key.char}`,
        block: key.block,
        onSelect: () => {
          if (key.kind === 'char') typeChar(key.char);
          else if (key.kind === 'backspace') backspace();
          else if (key.kind === 'done') deactivateSlot();
        },
      });
    }

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
      keypad.dispose();
      titleText.dispose();
      statusText.dispose();
      footerText.dispose();
      submitLabel.dispose();
      clearLabel.dispose();
      for (const t of slotTexts) t.dispose();
    },
  };
}
