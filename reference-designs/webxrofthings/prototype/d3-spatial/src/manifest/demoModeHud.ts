/**
 * demoModeHud — yellow pulsing "DEMO MODE" indicator card.
 *
 * Companion to `offlineSensorsHud`. When the manifest is feeding
 * fake data into one or more cells (because the real sensors are
 * offline), this card pulses to remind the user that what they're
 * looking at is synthesised — useful for screenshots, demos to
 * stakeholders, anywhere the difference between "live patient" and
 * "test pattern" matters.
 *
 * Visual contract:
 *  - Yellow background that pulses opacity 0.6 → 1.0 over ~1.4 s
 *  - "DEMO MODE" text in dark ink (high contrast on yellow)
 *  - Hidden when no marks are in fake-data mode
 *  - Pulse paused (mesh.visible=false) when hidden, so it costs
 *    nothing in healthy dataspaces
 */

import * as THREE from 'three';
import { Text } from 'troika-three-text';

const W              = 0.16;
const H              = 0.040;
const FILL_COLOR     = 0xffd84a;     // warm amber-yellow, distinct from the warn-orange palette
const TEXT_COLOR     = 0x1a1404;     // near-black, reads as ink on the yellow
const PULSE_PERIOD_S = 1.4;

export interface DemoModeHud {
  group: THREE.Group;
  /** Toggle the card on or off. Pulse animation pauses when off. */
  setVisible(on: boolean): void;
  /** Set the visible string (default "DEMO MODE"). Useful for
   *  variants like "DEMO MODE · 3/5 SENSORS" if a caller wants to
   *  surface how much of the dataspace is fake. */
  setLabel(text: string): void;
  dispose(): void;
}

export function buildDemoModeHud(): DemoModeHud {
  const group = new THREE.Group();
  group.name  = 'demo-mode-hud';
  group.visible = false;

  const fillMat = new THREE.MeshBasicMaterial({
    color: FILL_COLOR, transparent: true, opacity: 1.0, depthWrite: false,
  });
  const fillGeo = new THREE.PlaneGeometry(W, H);
  const fill    = new THREE.Mesh(fillGeo, fillMat);
  fill.position.z = -0.0005;
  group.add(fill);

  const label = new Text();
  label.text       = 'DEMO MODE';
  label.fontSize   = 0.018;
  label.color      = TEXT_COLOR;
  label.anchorX    = 'center';
  label.anchorY    = 'middle';
  label.position.set(0, 0, 0.001);
  label.sync();
  group.add(label);

  /* Pulse on fill.onBeforeRender so the cost is zero when the card
   * is culled (out of frustum) or hidden (setVisible(false)). */
  fill.onBeforeRender = () => {
    if (!group.visible) return;
    const t = performance.now() / 1000;
    /* Map sin -1..1 to a 0.55..1.0 opacity range — the floor keeps
     * the card legible at the bottom of the pulse rather than
     * fading to invisible. */
    const phase = Math.sin(t * (2 * Math.PI / PULSE_PERIOD_S));
    fillMat.opacity = 0.775 + 0.225 * phase;
  };

  function setVisible(on: boolean) {
    group.visible = on;
  }
  function setLabel(text: string) {
    if (label.text === text) return;
    label.text = text;
    label.sync();
  }
  function dispose() {
    fill.onBeforeRender = () => {};
    fillMat.dispose();
    fillGeo.dispose();
    (label as unknown as { dispose?: () => void }).dispose?.();
  }

  return { group, setVisible, setLabel, dispose };
}
