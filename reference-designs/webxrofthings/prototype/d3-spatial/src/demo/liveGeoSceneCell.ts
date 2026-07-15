/**
 * Live geo-scene cell — UC5 "pixel Japan" instantaneous-temperature map.
 *
 * A self-positioned, self-fetching mark (shape `geo-points`): a tile-grid
 * basemap (one square per prefecture, standard pixel-map arrangement) with
 * a temperature column rising from each tile. Column height AND color both
 * encode °C — redundant encoding for XR legibility; the tile top face tints
 * with the same scale so the map reads as a heat-mosaic at glancing angles.
 *
 * Color: diverging blue ↔ red around the current national mean, with a
 * DARK neutral midpoint — on a dark/passthrough surface the midpoint must
 * recede ("near the mean, unremarkable") and the extremes glow. Ramp
 * validated with the dataviz six-checks validator (both arms pass ordinal
 * checks on the dark surface; poles ΔE 87.9 under protan simulation).
 * Poles are deliberately brighter than the flat-categorical band: optical
 * waveguides attenuate dim/short-wavelength marks (see ui/palette.ts), and
 * column height is the secondary encoding.
 *
 * Polling follows the `targets` cell pattern (`SELF_FETCHING_SHAPES`): the
 * cell owns its fetch loop via startPolling; the manifest loader never
 * prefetches it. On fetch failure the last-good frame stays up and
 * getStatus() drives the offline badge; the mock server itself falls back
 * to simulation, so a blank map means the *dev server* is gone, not the
 * upstream.
 */

import * as THREE from 'three';
import { TEXT } from '../ui/palette';
import { startPolling, type PollingHandle } from './livePolling';
import { makeLabelSprite, type LabelSprite } from './labelSprite';
import type { LiveCell } from './liveVitalsCells';
import {
  JAPAN_PREFECTURES, JAPAN_GRID_COLS, JAPAN_GRID_ROWS,
  type JapanTempsPayload,
} from './japanPrefectures';

export interface LiveGeoSceneOpts {
  url: string;
  refreshMs?: number;
  /** Overall map width in metres (grid columns fit inside). Default 1.1. */
  width?: number;
  /** Tile footprint scaling: uniform squares, or area-scaled (Demers-style,
   *  each tile's footprint ∝ prefecture land area, anchored to its cell). */
  tileScale?: 'uniform' | 'area';
  /** Tabletop tilt in radians around X (leans the far edge away). Default ~35°. */
  tilt_rad?: number;
  /** World placement of the scene group (vizAnchor-local). */
  position?: { x?: number; y?: number; z?: number };
  /**
   * Deprecated — heights and colors both auto-fit the CURRENT national
   * min/max each poll (hottest = tallest; instantaneous readings cluster
   * within a few °C, so a fixed seasonal domain rendered every column at
   * near-identical height). Accepted and ignored for manifest back-compat.
   */
  domain?: [number, number];
  /** Max column height in metres at the current national max. Default 0.16. */
  maxColumnH?: number;
  /** Tween duration for value changes, ms. Default 1000. */
  tweenMs?: number;
}

/* Diverging ramp, cold → hot. Validated (see header). The midpoint is the
 * dark neutral from the reference palette's dark mode. */
const RAMP_STOPS = [0x4da3ff, 0x3f7fc7, 0x37578b, 0x383835, 0x7c4136, 0xc04434, 0xff5c3e];

/** Piecewise-linear interpolation over RAMP_STOPS. u in [0,1]. */
export function rampColor(u: number, out = new THREE.Color()): THREE.Color {
  const t = Math.min(1, Math.max(0, u)) * (RAMP_STOPS.length - 1);
  const i = Math.min(RAMP_STOPS.length - 2, Math.floor(t));
  const a = new THREE.Color(RAMP_STOPS[i]!);
  const b = new THREE.Color(RAMP_STOPS[i + 1]!);
  return out.copy(a).lerp(b, t - i);
}

const TILE_COLOR = 0x232526;      // resting tile face — near-surface, recessive
const TILE_EDGE_ALPHA = 0.35;

/* Color is diverging around the CURRENT national mean, and the ramp
 * stretches over the current spread (max |temp − mean|, floored) so the
 * coldest prefecture always reads blue and the hottest red — instantaneous
 * data clusters within a few °C of its mean, and mapping against the full
 * seasonal domain left the whole map sitting at the neutral midpoint
 * (caught in the first headless render). The floor stops sensor noise from
 * being amplified into full-scale color when the country is uniform. */
const MIN_COLOR_SPREAD_C = 3;

interface ColumnState {
  code: number;
  nameJa: string;
  /** Tile-center map-local coords (labels + glyphs anchor here). */
  baseX: number; baseY: number;
  column: THREE.Mesh;
  cap: THREE.Mesh;
  tileTop: THREE.MeshStandardMaterial;
  colMat: THREE.MeshStandardMaterial;
  capMat: THREE.MeshStandardMaterial;
  /** Kanji label hovering above the column (null in headless envs). */
  label: LabelSprite | null;
  /** Height + ramp-u tween state (from → to over tweenMs). */
  fromH: number; toH: number;
  fromU: number; toU: number;
  tweenStart: number;
  /** Trend tracking across polls. */
  lastTemp: number | null;
  trendDir: 0 | 1 | -1;
  trendAtMs: number;
}

export interface GeoSceneCell extends LiveCell {
  /** Show-only lifecycle: start/stop the poll loop (see applyShowOnly). */
  setActive(active: boolean): void;
}

export function buildLiveGeoSceneCell(opts: LiveGeoSceneOpts): GeoSceneCell {
  const {
    url,
    refreshMs = 300_000,
    width = 1.1,
    tileScale = 'uniform',
    tilt_rad = (35 * Math.PI) / 180,
    position,
    maxColumnH = 0.16,
    tweenMs = 1000,
  } = opts;

  const cellSize = width / JAPAN_GRID_COLS;
  const tileInset = cellSize * 0.08;           // 2px-spacer rule, scaled
  const depth = cellSize * JAPAN_GRID_ROWS;
  const MIN_COLUMN_H = 0.018;                  // coldest prefecture stays visible
  /* Trend arrows: show ▲/▼ after a reading moves by ≥ this much, and keep
   * the arrow up until the next change or TTL (JMA observes 10-minutely,
   * so a change is at most one arrow per obs window). */
  const TREND_MIN_DELTA_C = 0.1;
  const TREND_TTL_MS = 20 * 60_000;

  const group = new THREE.Group();
  group.name = `live-geo-scene:${url}`;

  /* Map plane group — tilted like a tabletop; legend stays on the group so
   * it tilts with the map (title/subtitle text stay upright via gallery). */
  const map = new THREE.Group();
  map.rotation.x = -tilt_rad;
  group.add(map);
  group.position.set(position?.x ?? 0, position?.y ?? 0.9, position?.z ?? -0.2);

  /* Ground plate under the grid — anchors the map visually. */
  const plate = new THREE.Mesh(
    new THREE.BoxGeometry(width + cellSize * 0.6, depth + cellSize * 0.6, 0.004),
    new THREE.MeshStandardMaterial({ color: 0x101112, roughness: 0.9, metalness: 0.0, transparent: true, opacity: 0.85 }),
  );
  plate.position.z = -0.004;
  map.add(plate);

  /* Grid → map-local coords. Grid y grows south; map-local +y is "up the
   * table" (north), so flip. Centered on the plate. */
  const cellXY = (gx: number, gy: number) => ({
    x: (gx + 0.5) * cellSize - width / 2,
    y: depth / 2 - (gy + 0.5) * cellSize,
  });

  /* Area scaling: tile side ∝ sqrt(area), normalized so the median
   * prefecture keeps ~the uniform footprint and the largest stays in-cell. */
  const areas = JAPAN_PREFECTURES.map(p => p.areaKm2).sort((a, b) => a - b);
  const medianArea = areas[Math.floor(areas.length / 2)]!;
  const maxArea = areas[areas.length - 1]!;
  const sideFor = (areaKm2: number): number => {
    if (tileScale !== 'area') return cellSize - tileInset;
    const rel = Math.sqrt(areaKm2 / medianArea);
    const relMax = Math.sqrt(maxArea / medianArea);
    // Median → 0.8 cells; largest (Hokkaido) capped at 1.25 cells.
    const side = cellSize * 0.8 * rel;
    return Math.min(side, cellSize * 1.25 * (rel / relMax) + cellSize * 0.8);
  };

  const columns = new Map<number, ColumnState>();
  const unitBox = new THREE.BoxGeometry(1, 1, 1);
  const capGeo = new THREE.SphereGeometry(1, 14, 10);

  for (const p of JAPAN_PREFECTURES) {
    const { x, y } = cellXY(p.gridX, p.gridY);
    const side = sideFor(p.areaKm2);

    const tileTop = new THREE.MeshStandardMaterial({
      color: TILE_COLOR, roughness: 0.85, metalness: 0.0,
      emissive: TILE_COLOR, emissiveIntensity: 0.15,
    });
    const tile = new THREE.Mesh(new THREE.BoxGeometry(side, side, 0.006), tileTop);
    tile.position.set(x, y, 0.003);
    map.add(tile);

    const edges = new THREE.LineSegments(
      new THREE.EdgesGeometry(tile.geometry),
      new THREE.LineBasicMaterial({ color: TEXT.dim, transparent: true, opacity: TILE_EDGE_ALPHA }),
    );
    edges.position.copy(tile.position);
    map.add(edges);

    const colMat = new THREE.MeshStandardMaterial({
      color: RAMP_STOPS[3], emissive: RAMP_STOPS[3], emissiveIntensity: 0.45,
      roughness: 0.4, metalness: 0.05, transparent: true, opacity: 0.92,
    });
    const column = new THREE.Mesh(unitBox, colMat);
    const colSide = side * 0.42;
    column.scale.set(colSide, colSide, 0.001);
    column.position.set(x, y, 0.006);
    map.add(column);

    const capMat = new THREE.MeshStandardMaterial({
      color: RAMP_STOPS[3], emissive: RAMP_STOPS[3], emissiveIntensity: 0.8,
      roughness: 0.3, metalness: 0.1,
    });
    const cap = new THREE.Mesh(capGeo, capMat);
    const capR = colSide * 0.62;
    cap.scale.setScalar(capR);
    cap.position.set(x, y, 0.006 + capR);
    cap.visible = false; // until first data lands
    map.add(cap);

    /* Kanji label hovering above the column top. Billboarded sprite —
     * reads correctly from any viewpoint; height follows the column in
     * tick(). Hidden until the first reading lands. */
    const label = makeLabelSprite({ height: cellSize * 0.34 });
    if (label) {
      label.sprite.position.set(x, y, 0.05);
      label.sprite.visible = false;
      map.add(label.sprite);
    }

    columns.set(p.code, {
      code: p.code, nameJa: p.nameJa, baseX: x, baseY: y,
      column, cap, tileTop, colMat, capMat, label,
      fromH: 0, toH: 0, fromU: 0.5, toU: 0.5, tweenStart: 0,
      lastTemp: null, trendDir: 0, trendAtMs: 0,
    });
  }

  /* ── Legend: canvas ramp strip + min/mid/max + source line ───────────
   * DOM-gated: headless/node test environments have no document; the cell
   * renders fine without the legend (same pattern as the imu cell's
   * browser-only globe). */
  const canUseDom = typeof document !== 'undefined';
  const legendCanvas = canUseDom ? document.createElement('canvas') : null;
  let legendTex: THREE.CanvasTexture | null = null;
  if (legendCanvas) {
    legendCanvas.width = 512; legendCanvas.height = 96;
    legendTex = new THREE.CanvasTexture(legendCanvas);
    legendTex.colorSpace = THREE.SRGBColorSpace;
    const legendMat = new THREE.MeshBasicMaterial({ map: legendTex, transparent: true });
    const legendW = width * 0.6;
    const legend = new THREE.Mesh(new THREE.PlaneGeometry(legendW, legendW * (96 / 512)), legendMat);
    legend.position.set(0, -depth / 2 - cellSize * 1.1, 0.002);
    map.add(legend);
  }

  let lastSource = 'simulated';
  let lastMeanC: number | null = null;
  let lastSpreadC = MIN_COLOR_SPREAD_C;

  function drawLegend() {
    const ctx = legendCanvas?.getContext('2d');
    if (!ctx || !legendTex) return;
    ctx.clearRect(0, 0, 512, 96);
    const gx0 = 16, gx1 = 496, gy0 = 12, gh = 26;
    const grad = ctx.createLinearGradient(gx0, 0, gx1, 0);
    const c = new THREE.Color();
    for (let i = 0; i <= 12; i++) {
      grad.addColorStop(i / 12, `#${rampColor(i / 12, c).getHexString()}`);
    }
    ctx.fillStyle = grad;
    ctx.fillRect(gx0, gy0, gx1 - gx0, gh);
    ctx.strokeStyle = 'rgba(184,163,128,0.6)'; // TEXT.muted
    ctx.strokeRect(gx0 + 0.5, gy0 + 0.5, gx1 - gx0 - 1, gh - 1);
    ctx.fillStyle = '#f5e9c8'; // TEXT.body
    ctx.font = '22px system-ui, sans-serif';
    ctx.textBaseline = 'top';
    /* Ramp endpoints are mean ± current spread (the color mapping), not the
     * fixed height domain. Before first data: generic cold/hot. */
    const m = lastMeanC;
    ctx.textAlign = 'left';
    ctx.fillText(m == null ? 'cold' : `${(m - lastSpreadC).toFixed(0)}°C`, gx0, gy0 + gh + 6);
    ctx.textAlign = 'right';
    ctx.fillText(m == null ? 'hot' : `${(m + lastSpreadC).toFixed(0)}°C`, gx1, gy0 + gh + 6);
    ctx.textAlign = 'center';
    const meanTxt = m == null ? '' : `mean ${m.toFixed(1)}°C · `;
    const srcTxt = lastSource === 'jma' ? '出典: 気象庁 (JMA)'
      : lastSource === 'open-meteo' ? 'Weather data by Open-Meteo.com'
      : 'SIMULATED';
    ctx.fillText(`${meanTxt}${srcTxt}`, (gx0 + gx1) / 2, gy0 + gh + 6);
    if (legendTex) legendTex.needsUpdate = true;
  }
  drawLegend();

  /* ── Data → tween targets ──────────────────────────────────────────── */
  function applyPayload(payload: JapanTempsPayload) {
    const temps = payload.stations.filter(s => s.tempC != null).map(s => s.tempC as number);
    const mean = temps.length ? temps.reduce((a, b) => a + b, 0) / temps.length : null;
    // Fallback midpoint is only reachable with zero reporting stations
    // (every column is hidden then anyway) — any finite value works.
    const m = mean ?? 15;
    const spread = Math.max(
      MIN_COLOR_SPREAD_C,
      ...temps.map(t => Math.abs(t - m)),
    );
    lastMeanC = mean;
    lastSpreadC = spread;
    lastSource = payload.source;
    /* Height: normalized to the CURRENT national min/max — hottest is
     * tallest, coldest is shortest, every poll. (A fixed seasonal domain
     * compressed same-moment readings into near-identical heights.) */
    const tMin = temps.length ? Math.min(...temps) : 0;
    const tMax = temps.length ? Math.max(...temps) : 1;
    const tSpan = (tMax - tMin) || 1;
    const now = performance.now();
    const wallNow = Date.now();
    for (const s of payload.stations) {
      const st = columns.get(s.code);
      if (!st) continue;
      st.fromH = st.toH;
      st.fromU = st.toU;
      if (s.tempC == null) {
        st.toH = 0.001;
        st.toU = 0.5;
        st.cap.visible = false;
        if (st.label) st.label.sprite.visible = false;
      } else {
        st.toH = MIN_COLUMN_H + ((s.tempC - tMin) / tSpan) * (maxColumnH - MIN_COLUMN_H);
        /* Color: relative to the current mean, arms scaled to the current
         * spread — coldest ≈ 0 (blue pole), hottest ≈ 1 (red pole). */
        st.toU = 0.5 + (s.tempC - m) / (2 * spread);
        st.cap.visible = true;

        /* Trend: arm the arrow when the reading moved vs the previous
         * DIFFERENT value; keep it until the next change or TTL. */
        if (st.lastTemp != null && Math.abs(s.tempC - st.lastTemp) >= TREND_MIN_DELTA_C) {
          st.trendDir = s.tempC > st.lastTemp ? 1 : -1;
          st.trendAtMs = wallNow;
        }
        st.lastTemp = s.tempC;
        if (st.trendDir !== 0 && wallNow - st.trendAtMs > TREND_TTL_MS) st.trendDir = 0;

        if (st.label) {
          const arrow = st.trendDir === 1 ? ' ▲' : st.trendDir === -1 ? ' ▼' : '';
          st.label.setText(`${st.nameJa} ${s.tempC.toFixed(1)}°${arrow}`);
          st.label.sprite.visible = true;
        }
      }
      st.tweenStart = now;
    }
    drawLegend();
  }

  async function refresh(): Promise<boolean> {
    try {
      const resp = await fetch(url);
      if (!resp.ok) return false;
      const payload = await resp.json() as JapanTempsPayload;
      if (!Array.isArray(payload.stations)) return false;
      applyPayload(payload);
      return true;
    } catch {
      return false; // keep last-good frame; status badge reports it
    }
  }

  /* Poll lifecycle follows the show-only protocol (see applyShowOnly in
   * main.ts): construct inactive, start polling on setActive(true), stop
   * on setActive(false). onLoaded activates marks per defaultVisible. */
  let poller: PollingHandle | null = null;
  function setActive(active: boolean) {
    if (active && !poller) {
      poller = startPolling(refreshMs, refresh);
    } else if (!active && poller) {
      poller.stop();
      poller = null;
    }
  }

  const tmpColor = new THREE.Color();
  function tick() {
    const now = performance.now();
    for (const st of columns.values()) {
      const k = st.tweenStart === 0 ? 1 : Math.min(1, (now - st.tweenStart) / tweenMs);
      const e = k * (2 - k); // easeOutQuad
      const h = st.fromH + (st.toH - st.fromH) * e;
      const u = st.fromU + (st.toU - st.fromU) * e;
      st.column.scale.z = Math.max(0.001, h);
      st.column.position.z = 0.006 + h / 2;
      rampColor(u, tmpColor);
      st.colMat.color.copy(tmpColor);
      st.colMat.emissive.copy(tmpColor);
      st.capMat.color.copy(tmpColor);
      st.capMat.emissive.copy(tmpColor);
      /* Tile top face tints at low intensity — the heat-mosaic read. */
      st.tileTop.emissive.copy(tmpColor);
      st.tileTop.emissiveIntensity = 0.22;
      const capR = st.cap.scale.x;
      st.cap.position.z = 0.006 + h + capR * 0.9;
      /* Extremes glow harder — |u-0.5| drives emissive on the cap. */
      st.capMat.emissiveIntensity = 0.5 + Math.min(0.5, Math.abs(u - 0.5)) * 1.4;
      /* Label hovers just above the cap, riding the height tween. */
      if (st.label) st.label.sprite.position.set(st.baseX, st.baseY, 0.006 + h + capR * 2.2 + 0.016);
    }
  }

  /* Self-driven animation via three's per-render hook — the manifest
   * pipeline has no per-frame tick (same rationale as liveImuCell's
   * fuselage.onBeforeRender). Fires whenever the plate renders; skipped
   * automatically while the cell is hidden or culled. The `tick()` method
   * below is kept for interface parity with the vitals cells + tests. */
  plate.onBeforeRender = () => { tick(); };

  return {
    group,
    tick,
    setActive,
    dispose: () => {
      poller?.stop();
      poller = null;
      for (const st of columns.values()) st.label?.dispose();
      legendTex?.dispose();
      group.traverse(o => {
        const m = o as THREE.Mesh;
        if ((m as any).isMesh || (m as any).isLineSegments) {
          m.geometry?.dispose?.();
          const mat = m.material as THREE.Material | THREE.Material[] | undefined;
          if (Array.isArray(mat)) mat.forEach(x => x.dispose());
          else mat?.dispose?.();
        }
      });
    },
    /* Dormant cells report 'live' (matches startPolling's pre-first-fetch
     * state) so a not-yet-activated mark doesn't trip the offline HUD. */
    getStatus: () => poller?.getStatus() ?? { state: 'live' as const, lastSuccessAgoMs: null },
  };
}
