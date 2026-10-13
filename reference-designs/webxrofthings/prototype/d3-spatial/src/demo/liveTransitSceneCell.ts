/**
 * Live transit-scene cell — UC6 Kumamoto city map with live vehicles.
 *
 * The `geo-scene` mark's second basemap: the route network IS the map
 * (docs/uc5-uc6-geo-dataspaces.md §2.3). A dark ground plate carries
 * per-operator route polylines (one merged LineSegments draw per operator),
 * stop dots, and bearing-oriented vehicle wedges whose positions tween
 * between polls so motion is continuous rather than teleporting.
 *
 * Layer sources:
 *  - network geometry: static snapshot (config.networkUrl → public/maps/…),
 *    fetched once at construction; committed asset, no upstream dependency.
 *  - vehicles: self-polled from data.url (`geo-points` shape → the loader
 *    never prefetches; same protocol as the UC5 cell). Phase 3 serves a
 *    deterministic simulation; Phase 4 swaps GTFS-RT behind the same URL
 *    and the `source` field flips the badge — zero client changes.
 *
 * Operator colors validated (dataviz six-checks, dark plate #101112): all
 * pairs pass CVD ≥ 12 and 3:1 contrast. The tram amber intentionally sits
 * above the categorical lightness band — it is the emphasized rail layer,
 * named in the legend (relief rule) and distinct in kind, not just hue.
 *
 * Lifecycle mirrors liveGeoSceneCell: construct inactive → setActive(true)
 * starts polling; animation self-drives via plate.onBeforeRender (the
 * manifest pipeline has no per-frame tick).
 */

import * as THREE from 'three';
import { TEXT } from '../ui/palette';
import { startPolling, type PollingHandle } from './livePolling';
import { makeLabelSprite, assignLabelTiers, type LabelSprite } from './labelSprite';
import type { LiveCell } from './liveVitalsCells';
import {
  makeCityProjection,
  type KumamotoNetwork, type KumamotoVehiclesPayload,
} from './kumamotoNetwork';

export interface LiveTransitSceneOpts {
  /** Vehicles feed (self-polled). */
  url: string;
  /** Static network snapshot (fetched once). */
  networkUrl: string;
  refreshMs?: number;
  /** Map width in metres (longer bbox side). Default 1.2. */
  width?: number;
  tilt_rad?: number;
  position?: { x?: number; y?: number; z?: number };
  /** Position tween duration; defaults to the refresh interval so vehicles
   *  arrive at the new fix right as the next one lands. */
  tweenMs?: number;
  /** Dim vehicles whose fix is older than this. Default 60 s. */
  staleAfterMs?: number;
  maxVehicles?: number;
}

/* Operator palette — validated, see header. */
const OPERATOR_COLORS: Record<string, number> = {
  tram: 0xffd97a,      // emphasized rail layer (TEXT.primary amber)
  toshibus: 0x3987e5,
  kumabus: 0x199e70,
  dentetsu: 0xd95926,
};
const FALLBACK_COLOR = 0x9085e9;

const PLATE_COLOR = 0x101112;

interface VehicleState {
  mesh: THREE.Mesh;
  mat: THREE.MeshStandardMaterial;
  /** Hovering "route · vehicle-id" label (null in headless envs). */
  label: LabelSprite | null;
  /** Vertical stacking tier when neighbors crowd (0 = base height). */
  labelTier: number;
  fromX: number; fromY: number; toX: number; toY: number;
  fromBearing: number; toBearing: number;
  tweenStart: number;
  lastTs: number;
  seen: boolean;
}

/** "toshibus-4001_1001_1_20260606-0" → readable tail for the label. */
function shortVehicleId(id: string, op: string): string {
  const bare = id.startsWith(`${op}-`) ? id.slice(op.length + 1) : id;
  // Fleet-number-style ids stay as-is; long synthetic ids keep the tail.
  return bare.length <= 8 ? bare : `…${bare.slice(-6)}`;
}

export interface TransitSceneCell extends LiveCell {
  setActive(active: boolean): void;
}

export function buildLiveTransitSceneCell(opts: LiveTransitSceneOpts): TransitSceneCell {
  const {
    url,
    networkUrl,
    refreshMs = 15_000,
    width = 1.2,
    tilt_rad = (35 * Math.PI) / 180,
    position,
    tweenMs = refreshMs,
    staleAfterMs = 60_000,
    maxVehicles = 64,
  } = opts;

  const group = new THREE.Group();
  group.name = `live-transit-scene:${url}`;
  const map = new THREE.Group();
  map.rotation.x = -tilt_rad;
  group.add(map);
  group.position.set(position?.x ?? 0, position?.y ?? 1.05, position?.z ?? -1.05);

  /* Plate sized once the network arrives; placeholder footprint until then. */
  const plateMat = new THREE.MeshStandardMaterial({
    color: PLATE_COLOR, roughness: 0.9, metalness: 0.0, transparent: true, opacity: 0.9,
  });
  const plate = new THREE.Mesh(new THREE.BoxGeometry(width, width * 0.8, 0.004), plateMat);
  plate.position.z = -0.004;
  map.add(plate);

  let project: ReturnType<typeof makeCityProjection> | null = null;
  const VEHICLE_Z = 0.012;

  /* ── network layer (once) ──────────────────────────────────────────── */
  let networkLoaded = false;
  const attributionBits: string[] = [];

  async function loadNetwork() {
    try {
      const resp = await fetch(networkUrl);
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const net = await resp.json() as KumamotoNetwork;
      project = makeCityProjection(net.bbox, width);

      // Re-fit the plate to the real aspect.
      plate.geometry.dispose();
      plate.geometry = new THREE.BoxGeometry(project.width + 0.06, project.height + 0.06, 0.004);

      for (const op of net.operators) {
        const color = OPERATOR_COLORS[op.key] ?? FALLBACK_COLOR;
        const isTram = op.kind === 'tram';

        // One merged LineSegments per operator — a single draw call.
        const verts: number[] = [];
        for (const r of op.routes) for (const line of r.lines) {
          for (let i = 1; i < line.length; i++) {
            const a = project.toXY(line[i - 1]![0], line[i - 1]![1]);
            const b = project.toXY(line[i]![0], line[i]![1]);
            // Tram rides slightly above the bus layer so it never z-fights.
            const z = isTram ? 0.0035 : 0.002;
            verts.push(a.x, a.y, z, b.x, b.y, z);
          }
        }
        const geo = new THREE.BufferGeometry();
        geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(verts), 3));
        const mat = new THREE.LineBasicMaterial({
          color, transparent: true, opacity: isTram ? 0.95 : 0.45,
        });
        map.add(new THREE.LineSegments(geo, mat));

        // Stops as world-unit points; tram stops read slightly larger.
        if (op.stops.length) {
          const sverts = new Float32Array(op.stops.length * 3);
          op.stops.forEach((s, i) => {
            const p = project!.toXY(s[0], s[1]);
            sverts[i * 3] = p.x; sverts[i * 3 + 1] = p.y; sverts[i * 3 + 2] = 0.003;
          });
          const sgeo = new THREE.BufferGeometry();
          sgeo.setAttribute('position', new THREE.BufferAttribute(sverts, 3));
          const smat = new THREE.PointsMaterial({
            color, size: isTram ? 0.008 : 0.004, sizeAttenuation: true,
            transparent: true, opacity: isTram ? 0.9 : 0.5,
          });
          map.add(new THREE.Points(sgeo, smat));
        }

        attributionBits.push(`${op.nameJa} (${op.license})`);
      }
      networkLoaded = true;
      drawLegend();
    } catch (e) {
      console.warn(`[transit-scene] network snapshot failed (${networkUrl}):`, e);
    }
  }
  void loadNetwork();

  /* ── vehicles layer ────────────────────────────────────────────────── */
  // Cone tip points +y at identity = bearing 0 (north); per-vehicle rotation
  // about z turns it clockwise to the live heading.
  const wedgeGeo = new THREE.ConeGeometry(0.008, 0.02, 6);
  /* Label sizing + neighbor stacking. Labels are ~4× wider than tall, so
   * the clustering bucket is label-width × label-height: vehicles whose
   * labels would overlap share a bucket and fan out vertically. */
  const LABEL_H = 0.03;
  const LABEL_W = LABEL_H * 5; // typical "line · fleet · speed" halo width
  const LABEL_BASE_LIFT = 0.026;
  const LABEL_TIER_STEP = LABEL_H * 1.15;
  const vehicles = new Map<string, VehicleState>();

  function vehicleFor(id: string, op: string): VehicleState {
    let v = vehicles.get(id);
    if (v) return v;
    const color = OPERATOR_COLORS[op] ?? FALLBACK_COLOR;
    const mat = new THREE.MeshStandardMaterial({
      color, emissive: color, emissiveIntensity: 0.85,
      roughness: 0.35, metalness: 0.05, transparent: true, opacity: 0.95,
    });
    const mesh = new THREE.Mesh(wedgeGeo, mat);
    mesh.visible = false;
    map.add(mesh);
    const label = makeLabelSprite({ height: LABEL_H, fontPx: 30 });
    if (label) {
      label.sprite.visible = false;
      map.add(label.sprite);
    }
    v = {
      mesh, mat, label, labelTier: 0,
      fromX: 0, fromY: 0, toX: 0, toY: 0,
      fromBearing: 0, toBearing: 0,
      tweenStart: 0, lastTs: 0, seen: false,
    };
    vehicles.set(id, v);
    return v;
  }

  let lastSource = 'simulated';
  let lastCount = 0;

  function applyPayload(payload: KumamotoVehiclesPayload) {
    if (!project) return; // network not in yet — next poll will land
    lastSource = payload.source;
    const now = performance.now();
    for (const v of vehicles.values()) v.seen = false;
    const list = payload.vehicles.slice(0, maxVehicles);
    for (const veh of list) {
      const st = vehicleFor(veh.id, veh.op);
      const p = project.toXY(veh.lon, veh.lat);
      if (!st.mesh.visible) {
        // First fix: appear in place, no cross-city swoosh.
        st.fromX = p.x; st.fromY = p.y;
        st.fromBearing = veh.bearing;
        st.mesh.visible = true;
      } else {
        st.fromX = st.toX; st.fromY = st.toY;
        st.fromBearing = st.toBearing;
      }
      st.toX = p.x; st.toY = p.y;
      st.toBearing = veh.bearing;
      st.tweenStart = now;
      st.lastTs = veh.ts;
      st.seen = true;
      if (st.label) {
        const line = veh.routeName || veh.routeId || '?';
        const fleet = veh.label || shortVehicleId(veh.id, veh.op);
        const speed = veh.speedKmh != null ? ` · ${veh.speedKmh} km/h` : '';
        st.label.setText(`${line} · ${fleet}${speed}`);
        st.label.sprite.visible = true;
      }
    }
    for (const v of vehicles.values()) {
      if (!v.seen) {
        v.mesh.visible = false; // vehicle left the feed
        if (v.label) v.label.sprite.visible = false;
      }
    }

    /* Neighbor-aware stacking: cluster by TARGET positions so labels that
     * will crowd each other by the end of this tween fan out vertically.
     * Stable id-sorted tiers keep a label on its tier between polls while
     * its neighborhood is unchanged. */
    const tierInput: Array<{ id: string; x: number; y: number }> = [];
    for (const [id, v] of vehicles) {
      if (v.seen) tierInput.push({ id, x: v.toX, y: v.toY });
    }
    const tiers = assignLabelTiers(tierInput, LABEL_W, LABEL_H);
    for (const [id, v] of vehicles) {
      if (v.seen) v.labelTier = tiers.get(id) ?? 0;
    }
    lastCount = list.length;
    drawLegend();
  }

  async function refresh(): Promise<boolean> {
    try {
      const resp = await fetch(url);
      if (!resp.ok) return false;
      const payload = await resp.json() as KumamotoVehiclesPayload;
      if (!Array.isArray(payload.vehicles)) return false;
      applyPayload(payload);
      return true;
    } catch {
      return false; // keep last-good frame; badge reports it
    }
  }

  let poller: PollingHandle | null = null;
  function setActive(active: boolean) {
    if (active && !poller) {
      poller = startPolling(refreshMs, refresh);
    } else if (!active && poller) {
      poller.stop();
      poller = null;
    }
  }

  /* ── legend ────────────────────────────────────────────────────────── */
  const canUseDom = typeof document !== 'undefined';
  const legendCanvas = canUseDom ? document.createElement('canvas') : null;
  let legendTex: THREE.CanvasTexture | null = null;
  const legend = new THREE.Mesh();
  if (legendCanvas) {
    legendCanvas.width = 1024; legendCanvas.height = 116;
    legendTex = new THREE.CanvasTexture(legendCanvas);
    legendTex.colorSpace = THREE.SRGBColorSpace;
    legend.geometry = new THREE.PlaneGeometry(width * 0.8, width * 0.8 * (116 / 1024));
    legend.material = new THREE.MeshBasicMaterial({ map: legendTex, transparent: true });
    legend.position.set(0, -(width * 0.8) / 2 - 0.10, 0.002);
    map.add(legend);
  }

  function drawLegend() {
    const ctx = legendCanvas?.getContext('2d');
    if (!ctx || !legendTex) return;
    ctx.clearRect(0, 0, 1024, 116);
    ctx.font = '30px system-ui, sans-serif';
    ctx.textBaseline = 'middle';
    // Operator chips, tram first.
    const entries: Array<[string, string]> = [
      ['tram', '市電'], ['toshibus', '都市バス'], ['kumabus', '熊本バス'], ['dentetsu', '電鉄バス'],
    ];
    let x = 12;
    for (const [key, label] of entries) {
      const c = new THREE.Color(OPERATOR_COLORS[key] ?? FALLBACK_COLOR);
      ctx.fillStyle = `#${c.getHexString()}`;
      ctx.fillRect(x, 18, 26, 26);
      ctx.fillStyle = '#f5e9c8';
      ctx.fillText(label, x + 34, 32);
      x += 34 + ctx.measureText(label).width + 28;
    }
    const badge = lastSource === 'gtfs-rt' ? `LIVE · ${lastCount} vehicles` : `SIMULATED · ${lastCount} vehicles`;
    ctx.fillStyle = lastSource === 'gtfs-rt' ? '#88ff99' : '#ffb873';
    ctx.textAlign = 'right';
    ctx.fillText(badge, 1012, 32);
    ctx.textAlign = 'left';
    ctx.fillStyle = '#b8a380';
    ctx.font = '22px system-ui, sans-serif';
    ctx.fillText('Data: 熊本市交通局 CC BY 2.1 JP · 熊本都市バス・熊本バス・熊本電鉄バス CC BY 4.0 (gtfs-data.jp / km.bus-vision.jp)', 12, 86);
    legendTex.needsUpdate = true;
  }
  drawLegend();

  /* ── per-frame animation (self-driven, see header) ─────────────────── */
  const upTmp = new THREE.Vector3(0, 0, 1);
  function tick() {
    const now = performance.now();
    for (const v of vehicles.values()) {
      if (!v.mesh.visible) continue;
      const k = v.tweenStart === 0 ? 1 : Math.min(1, (now - v.tweenStart) / tweenMs);
      const x = v.fromX + (v.toX - v.fromX) * k;
      const y = v.fromY + (v.toY - v.fromY) * k;
      v.mesh.position.set(x, y, VEHICLE_Z);
      // Shortest-arc bearing interpolation.
      let db = ((v.toBearing - v.fromBearing + 540) % 360) - 180;
      const bearing = v.fromBearing + db * k;
      // Bearing 0 = north (+y), clockwise → plane-local rotation about z.
      v.mesh.setRotationFromAxisAngle(upTmp, -(bearing * Math.PI) / 180);
      // Label rides above the wedge, following the tween; crowded
      // neighborhoods fan out vertically by tier.
      if (v.label) {
        v.label.sprite.position.set(x, y, VEHICLE_Z + LABEL_BASE_LIFT + v.labelTier * LABEL_TIER_STEP);
      }
      // Staleness dimming (label follows the wedge's opacity).
      const stale = v.lastTs > 0 && Date.now() - v.lastTs > staleAfterMs;
      v.mat.opacity = stale ? 0.35 : 0.95;
      v.mat.emissiveIntensity = stale ? 0.25 : 0.85;
      if (v.label) (v.label.sprite.material as THREE.SpriteMaterial).opacity = stale ? 0.35 : 1.0;
    }
  }
  plate.onBeforeRender = () => { tick(); };

  return {
    group,
    tick,
    setActive,
    dispose: () => {
      poller?.stop();
      poller = null;
      for (const v of vehicles.values()) v.label?.dispose();
      legendTex?.dispose();
      wedgeGeo.dispose();
      group.traverse(o => {
        const m = o as THREE.Mesh;
        if ((m as any).isMesh || (m as any).isLineSegments || (m as any).isPoints) {
          m.geometry?.dispose?.();
          const mat = m.material as THREE.Material | THREE.Material[] | undefined;
          if (Array.isArray(mat)) mat.forEach(x => x.dispose());
          else mat?.dispose?.();
        }
      });
    },
    getStatus: () => poller?.getStatus() ?? { state: 'live' as const, lastSuccessAgoMs: null },
  };
}
