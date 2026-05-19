/**
 * Live actuator-panel cell — UC2 in-XR home controls (P4c, P21, P22).
 *
 * The in-XR control surface for UC2's actuator devices. POSTs to
 * `/api/v1/actuator/*` and reflects returned state.
 *
 * Control kinds:
 *   - toggle  stateful single button (Light, Strip): shows "· ON"
 *             (green) / "· OFF" (dim), flips on press, reconciled to
 *             server truth on every refresh.
 *   - action  one-shot (Speaker chime/doorbell).
 *   - slider  segmented tap-slider for the thermostat setpoint: a row
 *             of cells across 16–26 °C in 0.5° steps; the active cell
 *             glows green, tap any cell to jump the setpoint there.
 *
 * Plus two read-only visuals:
 *   - a glowing heat/cool disc beside the thermostat (orange-red =
 *     heat, cyan-blue = cool, dim = off) with a soft pulse;
 *   - a per-device power-consumption breakdown (fake but state-driven
 *     watts: light ≈ 9 W×brightness, thermostat ≈ 1500 W heat /
 *     1200 cool / 2 W standby, strip ≈ 18 W×brightness, speaker ≈ 3 W)
 *     as mini bars + a total.
 *
 * UX rules honoured: every control is a real solid bordered mesh (no
 * invisible text buttons); bright-cyan hover focus + white press flash;
 * cherry-click on actuation. Plain THREE (not three-mesh-ui) so the
 * module stays node-test-safe in the builder import graph.
 *
 * Lazy: no network until setActive(true).
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
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!r.ok) { console.warn(`[actuator] POST ${path} → ${r.status}`); return null; }
    return await r.json();
  } catch (e) { console.warn(`[actuator] POST ${path} failed:`, e); return null; }
}
async function getJSON(path: string): Promise<any | null> {
  try {
    const r = await fetch(`${API}${path}`);
    return r.ok ? await r.json() : null;
  } catch { return null; }
}

// Palette
const BG_DEFAULT = 0x3a3530;
const BORDER_DEFAULT = 0x9a8a70;
const BG_HOVER = 0x2e5a78;
const BORDER_HOVER = 0x7fd1ff;
const BG_FLASH = 0x9fe4ff;
const BG_ON = 0x2e5a2e;
const BORDER_ON = 0x6cf0a0;

// Thermostat segmented slider range
const TEMP_MIN = 16;
const TEMP_MAX = 26;
const TEMP_STEP = 0.5;
const TEMP_CELLS = Math.round((TEMP_MAX - TEMP_MIN) / TEMP_STEP) + 1;   // 21

// Power model (fake, state-driven). Watts.
const PWR_MAX = 1600;   // bar full-scale (thermostat dominates)

interface StateBundle { light: any; thermo: any; strip: any }

export function buildLiveActuatorPanelCell(opts: { title?: string } = {}): LiveActuatorPanelCell {
  const group = new THREE.Group();
  group.name = 'live-actuator-panel';

  let active = false;
  let disposed = false;
  const labels: Text[] = [];
  const geos: THREE.BufferGeometry[] = [];
  const mats: THREE.Material[] = [];

  const PANEL_W = 0.62;
  const PANEL_H = 0.90;   // grew for Heat/Cool/Off row + 2 brightness sliders

  // ── Backdrop ───────────────────────────────────────────────────────
  const bgGeo = new THREE.PlaneGeometry(PANEL_W, PANEL_H);
  const bgMat = new THREE.MeshBasicMaterial({ color: 0x1a1815, transparent: true, opacity: 0.9 });
  const backdrop = new THREE.Mesh(bgGeo, bgMat);
  backdrop.position.set(0, 0, -0.003);
  backdrop.userData.noHover = true;
  group.add(backdrop);
  const bdBorderGeo = new THREE.EdgesGeometry(new THREE.PlaneGeometry(PANEL_W, PANEL_H));
  const bdBorderMat = new THREE.LineBasicMaterial({ color: 0xb8a380, transparent: true, opacity: 0.85 });
  const bdBorder = new THREE.LineSegments(bdBorderGeo, bdBorderMat);
  bdBorder.position.set(0, 0, -0.0025);
  group.add(bdBorder);

  function addText(s: string, x: number, y: number, size: number, color: number,
                   anchorX: 'left' | 'center' | 'right' = 'center'): Text {
    const t = new Text();
    t.text = s; t.fontSize = size; t.color = color;
    t.anchorX = anchorX; t.anchorY = 'middle';
    t.position.set(x, y, 0.004);
    t.sync();
    group.add(t); labels.push(t);
    return t;
  }

  // ── Top: title + status ────────────────────────────────────────────
  let cy = PANEL_H / 2 - 0.022;
  addText(opts.title ?? 'Room controls', 0, cy, 0.022, TEXT.primary);
  cy -= 0.032;
  const statusText = addText('idle', 0, cy, 0.012, TEXT.muted, 'center');
  statusText.maxWidth = PANEL_W - 0.04;
  function setStatus(s: string) { statusText.text = s; statusText.sync(); }

  // Shared button factory ────────────────────────────────────────────
  let hovered: THREE.Mesh | null = null;
  type Painter = () => void;
  interface Btn { fill: THREE.Mesh; fillMat: THREE.MeshBasicMaterial; bMat: THREE.LineBasicMaterial; }
  function makeButton(id: string, x: number, y: number, w: number, h: number, label: string): Btn {
    const fg = new THREE.PlaneGeometry(w, h);
    const fm = new THREE.MeshBasicMaterial({ color: BG_DEFAULT, transparent: true, opacity: 0.95 });
    const fill = new THREE.Mesh(fg, fm);
    fill.position.set(x, y, 0.001);
    fill.name = `actuator-btn:${id}`;
    group.add(fill);
    const bg = new THREE.EdgesGeometry(new THREE.PlaneGeometry(w, h));
    const bm = new THREE.LineBasicMaterial({ color: BORDER_DEFAULT, transparent: true, opacity: 0.8 });
    const bd = new THREE.LineSegments(bg, bm);
    bd.position.set(x, y, 0.0015);
    group.add(bd);
    geos.push(fg, bg); mats.push(fm, bm);
    if (label) addText(label, x, y, 0.0135, TEXT.body);
    return { fill, fillMat: fm, bMat: bm };
  }

  interface Entry {
    id: string; object: THREE.Object3D;
    onSelect: () => void; onHoverIn: () => void; onHoverOut: () => void;
    syncToggle?: (b: StateBundle) => void;
    syncSlider?: (b: StateBundle) => void;
  }
  const entries: Entry[] = [];

  /** Wire a button with shared hover/flash; `resting` repaints the
   *  non-focused look (state-aware for toggles/slider cells). */
  function wire(id: string, btn: Btn, resting: Painter, onPress: () => void) {
    const hover = () => { btn.fillMat.color.set(BG_HOVER); btn.fillMat.opacity = 1.0; btn.bMat.color.set(BORDER_HOVER); };
    resting();
    entries.push({
      id, object: btn.fill,
      onHoverIn: () => { hovered = btn.fill; hover(); playFocus(); },
      onHoverOut: () => { if (hovered === btn.fill) hovered = null; resting(); },
      onSelect: () => {
        btn.fillMat.color.set(BG_FLASH); btn.bMat.color.set(BG_FLASH); btn.fillMat.opacity = 1.0;
        playClick();
        setTimeout(() => { if (hovered === btn.fill) hover(); else resting(); }, 130);
        onPress();
      },
    });
  }

  // ── Row 1: Light + Strip toggles ───────────────────────────────────
  const BTN_H = 0.05;
  const TOG_W = 0.27;
  cy -= 0.058;
  {
    const lightOn = { v: false, known: false };
    const lb = makeButton('light', -PANEL_W / 4, cy, TOG_W, BTN_H, '');
    const lLbl = addText('Light · …', -PANEL_W / 4, cy, 0.0135, TEXT.body);
    const lResting: Painter = () => {
      if (lightOn.known && lightOn.v) { lb.fillMat.color.set(BG_ON); lb.bMat.color.set(BORDER_ON); }
      else { lb.fillMat.color.set(BG_DEFAULT); lb.bMat.color.set(BORDER_DEFAULT); }
      lb.fillMat.opacity = 0.95;
    };
    wire('light', lb, lResting, () => {
      const n = !lightOn.v; lightOn.v = n; lightOn.known = true;
      lLbl.text = `Light · ${n ? 'ON' : 'OFF'}`; lLbl.sync();
      setStatus(`Light → ${n ? 'ON' : 'OFF'}…`);
      void postJSON('/light', n ? { on: true, brightness_pct: 80 } : { on: false })
        .then(() => { if (!disposed) void refreshStatus(); });
    });
    entries[entries.length - 1]!.syncToggle = (b) => {
      const on = b.light?.on;
      if (typeof on !== 'boolean') return;
      lightOn.v = on; lightOn.known = true;
      lLbl.text = `Light · ${on ? 'ON' : 'OFF'}`; lLbl.sync();
      if (hovered !== lb.fill) lResting();
    };

    const stripOn = { v: false, known: false };
    const sb = makeButton('strip', PANEL_W / 4, cy, TOG_W, BTN_H, '');
    const sLbl = addText('Strip · …', PANEL_W / 4, cy, 0.0135, TEXT.body);
    const sResting: Painter = () => {
      if (stripOn.known && stripOn.v) { sb.fillMat.color.set(BG_ON); sb.bMat.color.set(BORDER_ON); }
      else { sb.fillMat.color.set(BG_DEFAULT); sb.bMat.color.set(BORDER_DEFAULT); }
      sb.fillMat.opacity = 0.95;
    };
    wire('strip', sb, sResting, () => {
      const n = !stripOn.v; stripOn.v = n; stripOn.known = true;
      sLbl.text = `Strip · ${n ? 'ON' : 'OFF'}`; sLbl.sync();
      setStatus(`Strip → ${n ? 'ON' : 'OFF'}…`);
      void postJSON('/neopixel', { on: n }).then(() => { if (!disposed) void refreshStatus(); });
    });
    entries[entries.length - 1]!.syncToggle = (b) => {
      const on = b.strip?.on;
      if (typeof on !== 'boolean') return;
      stripOn.v = on; stripOn.known = true;
      sLbl.text = `Strip · ${on ? 'ON' : 'OFF'}`; sLbl.sync();
      if (hovered !== sb.fill) sResting();
    };
  }

  // ── Brightness sliders: Light + Strip ──────────────────────────────
  //
  // Segmented "level-meter" sliders (0–100% in 10% steps): cells up to
  // the chosen level rest green, the rest dim. Reuses the same tap
  // machinery as the thermostat slider. Picking a level also turns the
  // device on (intuitive — you wouldn't set a brightness to leave it
  // off) so the change is visible AND the power bars finally move (the
  // power model scales by brightness_pct). The on/off toggles still
  // independently control on/off; both reconcile on refresh.
  const briSyncs: Array<(b: StateBundle) => void> = [];
  function makeBrightnessSlider(opts: {
    id: string;
    label: string;
    cy: number;
    post: (pct: number) => Promise<void>;
    readPct: (b: StateBundle) => number | undefined;
  }) {
    addText(opts.label, -PANEL_W / 2 + 0.04, opts.cy, 0.012, TEXT.body, 'left');
    const COUNT = 11;                       // 0,10,…,100
    const X0 = -0.16, X1 = 0.27;
    const GAP = 0.003;
    const CW = (X1 - X0 - GAP * (COUNT - 1)) / COUNT;
    const CH = 0.034;
    let activeIdx = -1;
    const cms: THREE.MeshBasicMaterial[] = [];
    const cbs: THREE.LineBasicMaterial[] = [];
    const cfs: THREE.Mesh[] = [];
    const paint = () => {
      for (let i = 0; i < COUNT; i++) {
        if (cfs[i] === hovered) continue;
        if (i <= activeIdx) { cms[i]!.color.set(BG_ON); cbs[i]!.color.set(BORDER_ON); }
        else { cms[i]!.color.set(BG_DEFAULT); cbs[i]!.color.set(BORDER_DEFAULT); }
        cms[i]!.opacity = 0.95;
      }
    };
    for (let i = 0; i < COUNT; i++) {
      const x = X0 + CW / 2 + i * (CW + GAP);
      const btn = makeButton(`${opts.id}-${i}`, x, opts.cy, CW, CH, '');
      cms.push(btn.fillMat); cbs.push(btn.bMat); cfs.push(btn.fill);
      const idx = i;
      const resting: Painter = () => {
        if (idx <= activeIdx) { btn.fillMat.color.set(BG_ON); btn.bMat.color.set(BORDER_ON); }
        else { btn.fillMat.color.set(BG_DEFAULT); btn.bMat.color.set(BORDER_DEFAULT); }
        btn.fillMat.opacity = 0.95;
      };
      wire(`${opts.id}-${i}`, btn, resting, () => {
        const pct = idx * 10;
        activeIdx = idx; paint();
        setStatus(`${opts.label} → ${pct}%…`);
        void opts.post(pct).then(() => { if (!disposed) void refreshStatus(); });
      });
    }
    briSyncs.push((b) => {
      const p = opts.readPct(b);
      if (typeof p !== 'number') return;
      activeIdx = Math.max(0, Math.min(COUNT - 1, Math.round(p / 10)));
      paint();
    });
  }

  cy -= 0.052;
  makeBrightnessSlider({
    id: 'lbri', label: 'Light  %', cy,
    post: (pct) => postJSON('/light', { on: pct > 0, brightness_pct: pct }).then(() => {}),
    readPct: (b) => b.light?.brightness_pct,
  });
  cy -= 0.052;
  makeBrightnessSlider({
    id: 'sbri', label: 'Strip  %', cy,
    post: (pct) => postJSON('/neopixel', { on: pct > 0, brightness_pct: pct }).then(() => {}),
    readPct: (b) => b.strip?.brightness_pct,
  });

  // ── Thermostat: header + glowing mode disc ─────────────────────────
  cy -= 0.072;
  const thermoHdr = addText('Thermostat   —', -0.06, cy, 0.016, TEXT.primary, 'center');

  // Glowing heat/cool disc (right of header).
  const discGeo = new THREE.CircleGeometry(0.022, 28);
  const discMat = new THREE.MeshBasicMaterial({ color: 0x3a3530, transparent: true, opacity: 0.95 });
  const disc = new THREE.Mesh(discGeo, discMat);
  disc.position.set(0.22, cy, 0.002);
  disc.userData.noHover = true;
  group.add(disc);
  geos.push(discGeo); mats.push(discMat);
  const discRingGeo = new THREE.EdgesGeometry(new THREE.CircleGeometry(0.024, 28));
  const discRingMat = new THREE.LineBasicMaterial({ color: 0x9a8a70, transparent: true, opacity: 0.7 });
  const discRing = new THREE.LineSegments(discRingGeo, discRingMat);
  discRing.position.set(0.22, cy, 0.0025);
  group.add(discRing);
  geos.push(discRingGeo); mats.push(discRingMat);
  const modeLbl = addText('off', 0.22, cy - 0.04, 0.010, TEXT.muted, 'center');
  // Disc colour by mode; soft pulse while heating/cooling.
  let discMode: 'heat' | 'cool' | 'off' = 'off';
  const DISC_HEAT = new THREE.Color(0xff5a2a);
  const DISC_COOL = new THREE.Color(0x4ec5ff);
  const DISC_OFF = new THREE.Color(0x3a3530);
  disc.onBeforeRender = () => {
    if (discMode === 'off') { discMat.color.copy(DISC_OFF); discMat.opacity = 0.85; return; }
    const base = discMode === 'heat' ? DISC_HEAT : DISC_COOL;
    // pulse ~0.6 Hz between 60% and 100% brightness
    const k = 0.6 + 0.4 * (0.5 + 0.5 * Math.sin(performance.now() / 1000 * Math.PI * 1.2));
    discMat.color.copy(base).multiplyScalar(k);
    discMat.opacity = 0.95;
  };

  // ── Thermostat mode selector: Heat / Cool / Off ────────────────────
  //
  // The glowing disc above only *reflected* mode — it was read-only.
  // These three buttons make it settable: the active mode rests green
  // (same affordance as the toggles), tap to POST {mode}. The disc +
  // these buttons reconcile to server truth on every refresh.
  cy -= 0.05;
  const MODE_W = 0.155, MODE_H = 0.042, MODE_GAP = 0.018;
  const modeDefs: Array<{ m: 'heat' | 'cool' | 'off'; label: string }> = [
    { m: 'heat', label: 'Heat' },
    { m: 'cool', label: 'Cool' },
    { m: 'off',  label: 'Off' },
  ];
  const modeBtns: Partial<Record<'heat' | 'cool' | 'off', Btn>> = {};
  function paintModes() {
    for (const d of modeDefs) {
      const b = modeBtns[d.m];
      if (!b || b.fill === hovered) continue;   // keep focus colour
      if (d.m === discMode) { b.fillMat.color.set(BG_ON); b.bMat.color.set(BORDER_ON); }
      else { b.fillMat.color.set(BG_DEFAULT); b.bMat.color.set(BORDER_DEFAULT); }
      b.fillMat.opacity = 0.95;
    }
  }
  modeDefs.forEach((d, k) => {
    const x = (k - 1) * (MODE_W + MODE_GAP);   // k=0,1,2 → centred triple
    const btn = makeButton(`mode-${d.m}`, x, cy, MODE_W, MODE_H, d.label);
    modeBtns[d.m] = btn;
    const resting: Painter = () => {
      if (d.m === discMode) { btn.fillMat.color.set(BG_ON); btn.bMat.color.set(BORDER_ON); }
      else { btn.fillMat.color.set(BG_DEFAULT); btn.bMat.color.set(BORDER_DEFAULT); }
      btn.fillMat.opacity = 0.95;
    };
    wire(`mode-${d.m}`, btn, resting, () => {
      discMode = d.m;            // optimistic — disc + buttons update now
      paintModes();
      setStatus(`Thermostat mode → ${d.label}…`);
      void postJSON('/thermostat', { mode: d.m }).then(() => { if (!disposed) void refreshStatus(); });
    });
  });

  // ── Thermostat segmented tap-slider ────────────────────────────────
  cy -= 0.05;
  const TRACK_X0 = -0.27, TRACK_X1 = 0.27;
  const TRACK_W = TRACK_X1 - TRACK_X0;
  const CELL_GAP = 0.002;
  const CELL_W = (TRACK_W - CELL_GAP * (TEMP_CELLS - 1)) / TEMP_CELLS;
  const CELL_H = 0.045;
  let setpointIdx = -1;   // active cell index; -1 until first read
  const cellMats: THREE.MeshBasicMaterial[] = [];
  const cellBorders: THREE.LineBasicMaterial[] = [];
  const cellFills: THREE.Mesh[] = [];

  function tempForIdx(i: number) { return TEMP_MIN + i * TEMP_STEP; }
  function idxForTemp(t: number) {
    return Math.max(0, Math.min(TEMP_CELLS - 1, Math.round((t - TEMP_MIN) / TEMP_STEP)));
  }
  function paintCells() {
    for (let i = 0; i < TEMP_CELLS; i++) {
      if (cellFills[i] === hovered) continue;   // keep focus colour
      if (i === setpointIdx) { cellMats[i]!.color.set(BG_ON); cellBorders[i]!.color.set(BORDER_ON); }
      else { cellMats[i]!.color.set(BG_DEFAULT); cellBorders[i]!.color.set(BORDER_DEFAULT); }
    }
  }
  for (let i = 0; i < TEMP_CELLS; i++) {
    const x = TRACK_X0 + CELL_W / 2 + i * (CELL_W + CELL_GAP);
    const btn = makeButton(`temp-${i}`, x, cy, CELL_W, CELL_H, '');
    cellMats.push(btn.fillMat); cellBorders.push(btn.bMat); cellFills.push(btn.fill);
    const idx = i;
    const resting: Painter = () => {
      if (idx === setpointIdx) { btn.fillMat.color.set(BG_ON); btn.bMat.color.set(BORDER_ON); }
      else { btn.fillMat.color.set(BG_DEFAULT); btn.bMat.color.set(BORDER_DEFAULT); }
      btn.fillMat.opacity = 0.95;
    };
    wire(`temp-${i}`, btn, resting, () => {
      const t = tempForIdx(idx);
      setpointIdx = idx;
      thermoHdr.text = `Thermostat   ${t.toFixed(1)}°C`; thermoHdr.sync();
      paintCells();
      setStatus(`Thermostat → ${t.toFixed(1)}°C…`);
      void postJSON('/thermostat', { setpoint_c: t }).then(() => { if (!disposed) void refreshStatus(); });
    });
    // Slider cells reconcile collectively in refreshStatus (see below).
  }
  cy -= 0.035;
  addText(`${TEMP_MIN}°`, TRACK_X0, cy, 0.011, TEXT.muted, 'left');
  addText(`${TEMP_MAX}°`, TRACK_X1, cy, 0.011, TEXT.muted, 'right');
  addText('tap to set setpoint', 0, cy, 0.010, TEXT.dim, 'center');

  // ── Speaker actions ────────────────────────────────────────────────
  cy -= 0.06;
  {
    const cb = makeButton('chime', -PANEL_W / 4, cy, TOG_W, BTN_H, 'Speaker · Chime');
    wire('chime', cb,
      () => { cb.fillMat.color.set(BG_DEFAULT); cb.bMat.color.set(BORDER_DEFAULT); cb.fillMat.opacity = 0.95; },
      () => { setStatus('Chime…'); void postJSON('/speaker/play', { sound_id: 'chime' }).then(() => { if (!disposed) void refreshStatus(); }); });
    const db = makeButton('doorbell', PANEL_W / 4, cy, TOG_W, BTN_H, 'Speaker · Doorbell');
    wire('doorbell', db,
      () => { db.fillMat.color.set(BG_DEFAULT); db.bMat.color.set(BORDER_DEFAULT); db.fillMat.opacity = 0.95; },
      () => { setStatus('Doorbell…'); void postJSON('/speaker/play', { sound_id: 'doorbell' }).then(() => { if (!disposed) void refreshStatus(); }); });
  }

  // ── Power consumption breakdown ────────────────────────────────────
  cy -= 0.062;
  addText('Power', -PANEL_W / 2 + 0.04, cy, 0.014, TEXT.primary, 'left');
  const totalLbl = addText('— W', PANEL_W / 2 - 0.04, cy, 0.014, TEXT.accent, 'right');
  cy -= 0.03;

  const PB_LABEL_X = -PANEL_W / 2 + 0.04;
  const PB_TRACK_X0 = -0.14;
  const PB_TRACK_W = 0.30;
  const PB_VAL_X = PANEL_W / 2 - 0.04;
  const PB_H = 0.018;
  interface PBar { fillMesh: THREE.Mesh; fillGeo: THREE.PlaneGeometry; val: Text; }
  function makePowerBar(name: string, y: number): PBar {
    addText(name, PB_LABEL_X, y, 0.011, TEXT.body, 'left');
    // track background
    const tg = new THREE.PlaneGeometry(PB_TRACK_W, PB_H);
    const tm = new THREE.MeshBasicMaterial({ color: 0x2a2520, transparent: true, opacity: 0.9 });
    const track = new THREE.Mesh(tg, tm);
    track.position.set(PB_TRACK_X0 + PB_TRACK_W / 2, y, 0.001);
    track.userData.noHover = true;
    group.add(track); geos.push(tg); mats.push(tm);
    // fill (anchored left; we scale + reposition as watts change)
    const fgg = new THREE.PlaneGeometry(1, PB_H);   // unit width, scaled in update
    const fmm = new THREE.MeshBasicMaterial({ color: 0xffb04a, transparent: true, opacity: 0.95 });
    const fmesh = new THREE.Mesh(fgg, fmm);
    fmesh.position.set(PB_TRACK_X0, y, 0.0015);
    fmesh.scale.x = 0.0001;
    fmesh.userData.noHover = true;
    group.add(fmesh); geos.push(fgg); mats.push(fmm);
    const val = addText('0 W', PB_VAL_X, y, 0.011, TEXT.muted, 'right');
    return { fillMesh: fmesh, fillGeo: fgg, val };
  }
  const pbLight = makePowerBar('Light', cy);   cy -= 0.032;
  const pbThermo = makePowerBar('Thermostat', cy); cy -= 0.032;
  const pbStrip = makePowerBar('Strip', cy);   cy -= 0.032;
  const pbSpeaker = makePowerBar('Speaker', cy);

  function setBar(pb: PBar, watts: number) {
    const frac = Math.max(0, Math.min(1, watts / PWR_MAX));
    const w = Math.max(0.0001, frac * PB_TRACK_W);
    pb.fillMesh.scale.x = w;
    pb.fillMesh.position.x = PB_TRACK_X0 + w / 2;
    pb.val.text = `${Math.round(watts)} W`;
    pb.val.sync();
  }

  // ── State refresh ──────────────────────────────────────────────────
  async function refreshStatus() {
    const [light, thermo, strip] = await Promise.all([
      getJSON('/light'), getJSON('/thermostat'), getJSON('/neopixel'),
    ]);
    if (disposed) return;
    const b: StateBundle = { light, thermo, strip };

    for (const e of entries) e.syncToggle?.(b);
    for (const s of briSyncs) s(b);   // reconcile Light/Strip brightness sliders

    if (thermo) {
      thermoHdr.text = `Thermostat   ${Number(thermo.setpoint_c).toFixed(1)}°C`;
      thermoHdr.sync();
      setpointIdx = idxForTemp(Number(thermo.setpoint_c));
      paintCells();
      discMode = (thermo.mode === 'heat' || thermo.mode === 'cool') ? thermo.mode : 'off';
      modeLbl.text = `${thermo.mode}  ${Number(thermo.current_c).toFixed(1)}°C`;
      modeLbl.sync();
      paintModes();   // reconcile the Heat/Cool/Off buttons to server truth
    }

    // Fake but state-driven power model.
    const lW = light?.on ? 9 * (Number(light.brightness_pct ?? 100) / 100) : 0;
    const tW = thermo ? (thermo.mode === 'heat' ? 1500 : thermo.mode === 'cool' ? 1200 : 2) : 0;
    const sW = strip?.on ? 18 * (Number(strip.brightness_pct ?? 100) / 100) : 0;
    const spW = 3;   // speaker idle/standby
    setBar(pbLight, lW); setBar(pbThermo, tW); setBar(pbStrip, sW); setBar(pbSpeaker, spW);
    const total = lW + tW + sW + spW;
    totalLbl.text = `${Math.round(total)} W`; totalLbl.sync();

    const parts: string[] = [];
    if (light) parts.push(`Light ${light.on ? `${light.brightness_pct}%` : 'off'}`);
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
    tick: (_t: number) => { /* disc pulse self-drives via onBeforeRender */ },
    dispose: () => {
      disposed = true;
      disc.onBeforeRender = () => {};
      for (const l of labels) l.dispose();
      labels.length = 0;
      for (const g of geos) g.dispose();
      for (const m of mats) m.dispose();
    },
  };
}
