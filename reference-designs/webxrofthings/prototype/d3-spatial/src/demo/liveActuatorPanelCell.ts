/**
 * Live actuator-panel cell — UC2 in-XR home controls (P4c).
 *
 * UC2 ships actuator *devices* (smart bulb, thermostat, speaker,
 * NeoPixel strip) with working POST endpoints on the mock-join-server,
 * but had no in-XR way to drive them. This cell is that control
 * surface: a panel of real, visible, pressable buttons that POST to
 * `/api/v1/actuator/*` and reflect the returned state in a status line.
 *
 * Two control kinds:
 *   - action  one-shot: Thermostat ±, Speaker chime/doorbell.
 *   - toggle  stateful: Light, Strip. ONE button that shows its current
 *             state (green "· ON" / dim "· OFF"), flips on press, and
 *             reconciles to server truth after every refresh. Replaces
 *             the old separate On/Off pair — clearer and saves a row.
 *
 * Deliberate UX per direction:
 *   - Every control is a real solid mesh with a visible border — NO
 *     invisible text-only "buttons".
 *   - Clear focus: bright-cyan fill + border on hover; near-white flash
 *     on press. Toggles additionally colour their *resting* state
 *     (green when ON) so on/off is readable at a glance without focus.
 *   - Actuation plays the cherry-click from uiSounds.
 *
 * Built from plain THREE meshes (not three-mesh-ui) on purpose: this
 * module is in the manifest-builder import graph, and three-mesh-ui's
 * UMD build touches a global `THREE` at import time which blows up the
 * node test runner. Plain Plane + EdgesGeometry + troika text is
 * node-safe and gives direct control over the hover/flash colours.
 *
 * Lazy like the other cells: no network until setActive(true).
 */

import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';
import { playClick, playFocus } from '../audio/uiSounds';

export interface LiveActuatorPanelCell {
  group: THREE.Group;
  getInteractables(): Array<{
    id: string;
    object: THREE.Object3D;
    onSelect: () => void;
    onHoverIn?: () => void;
    onHoverOut?: () => void;
  }>;
  setActive(active: boolean): void;
  tick(time: number): void;
  dispose(): void;
}

const API = '/api/v1/actuator';

async function postJSON(path: string, body: unknown): Promise<unknown | null> {
  try {
    const r = await fetch(`${API}${path}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!r.ok) { console.warn(`[actuator] POST ${path} → ${r.status}`); return null; }
    return await r.json();
  } catch (e) {
    console.warn(`[actuator] POST ${path} failed:`, e);
    return null;
  }
}
async function getJSON(path: string): Promise<any | null> {
  try {
    const r = await fetch(`${API}${path}`);
    if (!r.ok) return null;
    return await r.json();
  } catch { return null; }
}

// Palette
const BG_DEFAULT = 0x3a3530;
const BORDER_DEFAULT = 0x9a8a70;
const BG_HOVER = 0x2e5a78;
const BORDER_HOVER = 0x7fd1ff;
const BG_FLASH = 0x9fe4ff;
const BG_ON = 0x2e5a2e;          // toggle resting fill when ON
const BORDER_ON = 0x6cf0a0;      // toggle resting border when ON

interface StateBundle { light: any; thermo: any; strip: any }

type Control =
  | { kind: 'action'; id: string; label: string; run: () => Promise<void> }
  | {
      kind: 'toggle';
      id: string;
      /** Prefix shown before " · ON"/" · OFF". */
      label: string;
      /** POST the requested on/off state. */
      post: (on: boolean) => Promise<void>;
      /** Pull this toggle's truth out of the refresh bundle. */
      readOn: (b: StateBundle) => boolean | undefined;
    };

export function buildLiveActuatorPanelCell(opts: { title?: string } = {}): LiveActuatorPanelCell {
  const group = new THREE.Group();
  group.name = 'live-actuator-panel';

  let active = false;
  let disposed = false;
  const labels: Text[] = [];

  const PANEL_W = 0.54;
  const PANEL_H = 0.36;            // one row shorter than before (toggles merged pairs)

  const bgGeo = new THREE.PlaneGeometry(PANEL_W, PANEL_H);
  const bgMat = new THREE.MeshBasicMaterial({ color: 0x1a1815, transparent: true, opacity: 0.9 });
  const backdrop = new THREE.Mesh(bgGeo, bgMat);
  backdrop.position.set(0, 0, -0.003);
  backdrop.userData.noHover = true;
  group.add(backdrop);

  const borderGeo = new THREE.EdgesGeometry(new THREE.PlaneGeometry(PANEL_W, PANEL_H));
  const borderMat = new THREE.LineBasicMaterial({ color: 0xb8a380, transparent: true, opacity: 0.85 });
  const backdropBorder = new THREE.LineSegments(borderGeo, borderMat);
  backdropBorder.position.set(0, 0, -0.0025);
  group.add(backdropBorder);

  const titleText = new Text();
  titleText.text = opts.title ?? 'Room controls';
  titleText.fontSize = 0.022;
  titleText.color = TEXT.primary;
  titleText.anchorX = 'center';
  titleText.anchorY = 'top';
  titleText.position.set(0, PANEL_H / 2 - 0.018, 0.004);
  titleText.sync();
  group.add(titleText);
  labels.push(titleText);

  const statusText = new Text();
  statusText.text = 'idle';
  statusText.fontSize = 0.013;
  statusText.color = TEXT.muted;
  statusText.anchorX = 'center';
  statusText.anchorY = 'top';
  statusText.position.set(0, PANEL_H / 2 - 0.05, 0.004);
  statusText.maxWidth = PANEL_W - 0.04;
  statusText.sync();
  group.add(statusText);
  labels.push(statusText);

  function setStatus(s: string) { statusText.text = s; statusText.sync(); }

  const controls: Control[] = [
    {
      kind: 'toggle', id: 'light', label: 'Light',
      post: async (on) => { await postJSON('/light', on ? { on: true, brightness_pct: 80 } : { on: false }); },
      readOn: (b) => b.light?.on,
    },
    {
      kind: 'toggle', id: 'strip', label: 'Strip',
      post: async (on) => { await postJSON('/neopixel', { on }); },
      readOn: (b) => b.strip?.on,
    },
    { kind: 'action', id: 'warmer', label: 'Thermostat  +',
      run: async () => {
        const t = await getJSON('/thermostat');
        await postJSON('/thermostat', { setpoint_c: (t?.setpoint_c ?? 21) + 0.5 });
      } },
    { kind: 'action', id: 'cooler', label: 'Thermostat  −',
      run: async () => {
        const t = await getJSON('/thermostat');
        await postJSON('/thermostat', { setpoint_c: (t?.setpoint_c ?? 21) - 0.5 });
      } },
    { kind: 'action', id: 'chime', label: 'Speaker · Chime',
      run: async () => { await postJSON('/speaker/play', { sound_id: 'chime' }); } },
    { kind: 'action', id: 'doorbell', label: 'Speaker · Doorbell',
      run: async () => { await postJSON('/speaker/play', { sound_id: 'doorbell' }); } },
  ];

  type Entry = {
    id: string; object: THREE.Object3D;
    onSelect: () => void; onHoverIn: () => void; onHoverOut: () => void;
    /** toggle-only: reconcile resting visual to a fetched on/off state. */
    syncToggle?: (b: StateBundle) => void;
  };
  const entries: Entry[] = [];
  const btnGeos: THREE.BufferGeometry[] = [];
  const btnMats: THREE.Material[] = [];
  let hovered: THREE.Mesh | null = null;

  const BTN_W = 0.245;
  const BTN_H = 0.05;
  const COL_GAP = 0.02;
  const ROW_GAP = 0.014;
  const cols = 2;
  const startY = PANEL_H / 2 - 0.085;

  controls.forEach((ctrl, i) => {
    const col = i % cols;
    const row = Math.floor(i / cols);
    const x = (col - (cols - 1) / 2) * (BTN_W + COL_GAP);
    const y = startY - row * (BTN_H + ROW_GAP) - BTN_H / 2;

    const fillGeo = new THREE.PlaneGeometry(BTN_W, BTN_H);
    const fillMat = new THREE.MeshBasicMaterial({ color: BG_DEFAULT, transparent: true, opacity: 0.95 });
    const fill = new THREE.Mesh(fillGeo, fillMat);
    fill.position.set(x, y, 0.001);
    fill.name = `actuator-btn:${ctrl.id}`;
    group.add(fill);

    const bGeo = new THREE.EdgesGeometry(new THREE.PlaneGeometry(BTN_W, BTN_H));
    const bMat = new THREE.LineBasicMaterial({ color: BORDER_DEFAULT, transparent: true, opacity: 0.8 });
    const border = new THREE.LineSegments(bGeo, bMat);
    border.position.set(x, y, 0.0015);
    group.add(border);

    const lbl = new Text();
    lbl.text = ctrl.kind === 'toggle' ? `${ctrl.label} · …` : ctrl.label;
    lbl.fontSize = 0.014;
    lbl.color = TEXT.body;
    lbl.anchorX = 'center';
    lbl.anchorY = 'middle';
    lbl.position.set(x, y, 0.004);
    lbl.sync();
    group.add(lbl);

    labels.push(lbl);
    btnGeos.push(fillGeo, bGeo);
    btnMats.push(fillMat, bMat);

    if (ctrl.kind === 'toggle') {
      let currentOn = false;
      let known = false;   // until first server read, show "· …"
      const paintResting = () => {
        if (!known) { fillMat.color.set(BG_DEFAULT); fillMat.opacity = 0.95; bMat.color.set(BORDER_DEFAULT); return; }
        if (currentOn) { fillMat.color.set(BG_ON); fillMat.opacity = 0.95; bMat.color.set(BORDER_ON); }
        else { fillMat.color.set(BG_DEFAULT); fillMat.opacity = 0.95; bMat.color.set(BORDER_DEFAULT); }
      };
      const setLabel = () => {
        lbl.text = `${ctrl.label} · ${known ? (currentOn ? 'ON' : 'OFF') : '…'}`;
        lbl.sync();
      };
      const toHover = () => { fillMat.color.set(BG_HOVER); fillMat.opacity = 1.0; bMat.color.set(BORDER_HOVER); };

      entries.push({
        id: ctrl.id,
        object: fill,
        onHoverIn: () => { hovered = fill; toHover(); playFocus(); },
        onHoverOut: () => { if (hovered === fill) hovered = null; paintResting(); },
        onSelect: () => {
          const next = !currentOn;
          fillMat.color.set(BG_FLASH); bMat.color.set(BG_FLASH); fillMat.opacity = 1.0;
          playClick();
          // Optimistic: reflect the requested state immediately.
          currentOn = next; known = true; setLabel();
          setTimeout(() => { if (hovered === fill) toHover(); else paintResting(); }, 130);
          setStatus(`${ctrl.label} → ${next ? 'ON' : 'OFF'}…`);
          void ctrl.post(next).then(() => { if (!disposed) void refreshStatus(); });
        },
        syncToggle: (b) => {
          const on = ctrl.readOn(b);
          if (typeof on !== 'boolean') return;
          currentOn = on; known = true;
          setLabel();
          if (hovered !== fill) paintResting();
        },
      });
    } else {
      const toDefault = () => { fillMat.color.set(BG_DEFAULT); fillMat.opacity = 0.95; bMat.color.set(BORDER_DEFAULT); };
      const toHover = () => { fillMat.color.set(BG_HOVER); fillMat.opacity = 1.0; bMat.color.set(BORDER_HOVER); };
      entries.push({
        id: ctrl.id,
        object: fill,
        onHoverIn: () => { hovered = fill; toHover(); playFocus(); },
        onHoverOut: () => { if (hovered === fill) hovered = null; toDefault(); },
        onSelect: () => {
          fillMat.color.set(BG_FLASH); bMat.color.set(BG_FLASH); fillMat.opacity = 1.0;
          playClick();
          setTimeout(() => { if (hovered === fill) toHover(); else toDefault(); }, 130);
          setStatus(`${ctrl.label}…`);
          void ctrl.run().then(() => { if (!disposed) void refreshStatus(); });
        },
      });
    }
  });

  async function refreshStatus() {
    const [light, thermo, strip] = await Promise.all([
      getJSON('/light'), getJSON('/thermostat'), getJSON('/neopixel'),
    ]);
    if (disposed) return;
    const bundle: StateBundle = { light, thermo, strip };
    // Reconcile every toggle's resting visual + label to server truth.
    for (const e of entries) e.syncToggle?.(bundle);
    const parts: string[] = [];
    if (light) parts.push(`Light ${light.on ? `${light.brightness_pct}%` : 'off'}`);
    if (thermo) parts.push(`Thermo ${thermo.setpoint_c}°C → ${thermo.current_c}°C ${thermo.mode}`);
    if (strip) parts.push(`Strip ${strip.on ? 'on' : 'off'}`);
    setStatus(parts.join('   ·   ') || 'no actuator response');
  }

  return {
    group,
    getInteractables: () => entries.map(e => ({
      id: e.id, object: e.object,
      onSelect: e.onSelect, onHoverIn: e.onHoverIn, onHoverOut: e.onHoverOut,
    })),
    setActive(next: boolean) {
      if (next === active) return;
      active = next;
      if (active) void refreshStatus();
    },
    tick: (_t: number) => { /* event-driven; nothing per-frame */ },
    dispose: () => {
      disposed = true;
      for (const l of labels) l.dispose();
      labels.length = 0;
      bgGeo.dispose(); bgMat.dispose();
      borderGeo.dispose(); borderMat.dispose();
      for (const g of btnGeos) g.dispose();
      for (const m of btnMats) m.dispose();
    },
  };
}
