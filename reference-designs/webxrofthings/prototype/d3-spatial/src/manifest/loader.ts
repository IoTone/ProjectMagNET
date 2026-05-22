/**
 * Manifest Loader v1
 *
 * Reads a DataspaceManifest and instantiates viz builders for each mark.
 * This decouples data from rendering — any dataspace that publishes a
 * conformant manifest gets a fully interactive spatial visualization
 * without per-dataset code changes.
 */

import * as THREE from 'three';
import type { DataspaceManifest, MarkSpec, MarkType } from './schema';
import type { HNode } from '../demo/sampleHierarchy';
import { createHealthMonitor, type HealthMonitor } from './healthMonitor';
import { generateFakePayload } from './fakeData';

export interface LoadedMark {
  id: string;
  type: MarkType;
  title: string;
  subtitle?: string;
  group: THREE.Group;
  viz: unknown;
  drillable: boolean;
  hoverable: boolean;
  draggable: boolean;
  /** From `MarkSpec.defaultVisible`; default true. renderManifest toggles
   *  the cell's `visible` flag accordingly so the HUD handler can flip
   *  it without re-loading. */
  defaultVisible: boolean;
  /**
   * Optional updater. The loader calls this on the cadence specified by the
   * mark's `data.refreshInterval` (seconds). The spec passed in already has
   * its `data` field re-mutated to inline form with the latest fetched payload.
   */
  refresh?: (spec: MarkSpec) => void;
}

export interface LoadResult {
  name: string;
  scaleTag: string;
  marks: LoadedMark[];
  /** Per-mark fetch health. renderManifest reads this to build the
   *  offline-sensors + DEMO-mode HUDs; the loader's setInterval body
   *  pushes fake data into offline marks. Always present, even for
   *  manifests with no URL marks (just stays empty). */
  health: HealthMonitor;
  /**
   * Stop all scheduled refresh intervals. Call on dataspace teardown to avoid
   * stranded setInterval()s polling forever.
   */
  dispose: () => void;
}

type Builder = (spec: MarkSpec) => LoadedMark | null;

const builders = new Map<MarkType, Builder>();

/** Register a builder for a mark type. Called once at startup per mark type. */
export function registerMarkBuilder(type: MarkType, builder: Builder) {
  builders.set(type, builder);
}

/** Load a manifest and instantiate all its marks. Always creates a
 *  HealthMonitor (returned on LoadResult) — the monitor receives
 *  per-mark fetch outcomes on every initial + periodic refresh and
 *  gets queried to decide whether the next setInterval tick should
 *  hand fake data into the builder's refresh callback (offline →
 *  DEMO MODE; back to real data on recovery). renderManifest
 *  subscribes to it to drive the offline-sensors + DEMO HUDs. */
export async function loadManifest(
  manifest: DataspaceManifest,
  token?: string,
): Promise<LoadResult> {
  const marks: LoadedMark[] = [];
  const intervals: Array<ReturnType<typeof setInterval>> = [];
  const health = createHealthMonitor();

  /** Re-fetch a URL data source and mutate spec.data with the new inline data.
   *  Returns true on success. Bounded by a 5 s AbortController timeout so an
   *  offline / hung device endpoint can't block the rest of the manifest
   *  load — without this, a single unreachable URL stalls the for-loop in
   *  loadManifest indefinitely and the scene never renders ("blank, no
   *  error" because failures are logged to console, not visible UI). */
  /** Stamp a fake payload into `spec.data` if `fakeData.ts` knows how to
   *  synthesise this shape. Returns true if the spec was mutated (the
   *  caller should re-run the builder's refresh callback). Keeps the
   *  refresh-loop body small + readable. */
  function seedFakeIfPossible(spec: MarkSpec, shape: string): boolean {
    const fake = generateFakePayload(spec, shape);
    if (fake == null) return false;
    (spec.data as any) = { source: 'inline', ...shapeToField(shape, fake) };
    return true;
  }

  async function fetchInto(spec: MarkSpec, url: string, shape: string): Promise<boolean> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 5000);
    try {
      const headers: Record<string, string> = {};
      if (token) headers['Authorization'] = `Bearer ${token}`;
      const resp = await fetch(url, { headers, signal: controller.signal });
      if (!resp.ok) {
        console.warn(`[manifest] ${url} → ${resp.status}`);
        return false;
      }
      const json = await resp.json();
      (spec.data as any) = { source: 'inline', ...shapeToField(shape, json) };
      return true;
    } catch (e) {
      console.warn(`[manifest] failed to fetch ${url}:`, e);
      return false;
    } finally {
      clearTimeout(timer);
    }
  }

  /* Pre-fetch every URL-source mark IN PARALLEL via Promise.allSettled,
   * so one offline endpoint doesn't gate the others. Each individual
   * fetchInto already has a 5 s timeout, so the worst-case wait for the
   * whole manifest is ~5 s regardless of how many marks are unreachable.
   * (Sequentially this was N×5 s — e.g. env down → temp + humidity =
   * 10 s before the camera mark even got a chance to render.) */
  const SELF_FETCHING_SHAPES = new Set(['video', 'imu']);
  const urlSpecs = new Map<MarkSpec, { url: string; shape: string; refreshInterval: number }>();
  const prefetchJobs: Promise<unknown>[] = [];
  for (const spec of manifest.marks) {
    if (spec.data.source !== 'url') continue;
    const urlData = spec.data;
    const url = urlData.url;
    if (url.startsWith('wss://') || url.startsWith('ws://')) continue;
    urlSpecs.set(spec, {
      url, shape: urlData.shape, refreshInterval: urlData.refreshInterval ?? 0,
    });
    /* Register with the health monitor up front. The refreshInterval
     * drives the staleness window; if the manifest didn't specify one
     * use a conservative 30 s default so the offline-detection clock
     * doesn't fire too eagerly. */
    health.register(spec.id, urlData.refreshInterval ?? 30);
    if (!SELF_FETCHING_SHAPES.has(urlData.shape)) {
      prefetchJobs.push(
        fetchInto(spec, url, urlData.shape).then((ok) => {
          health.recordFetch(spec.id, ok);
          /* Pre-fetch failure: seed the spec with a fake payload so the
           * builder still has something to render. The user sees the
           * DEMO HUD light up almost immediately on a cold-start when
           * the device is unreachable. */
          if (!ok) seedFakeIfPossible(spec, urlData.shape);
        })
      );
    }
  }
  await Promise.allSettled(prefetchJobs);

  for (const spec of manifest.marks) {
    /* WebSocket sources noted in the original loop — preserve the warn. */
    if (spec.data.source === 'url' &&
        (spec.data.url.startsWith('wss://') || spec.data.url.startsWith('ws://'))) {
      console.warn(`[manifest] skipping WebSocket source ${spec.data.url} (not yet connected)`);
      continue;
    }
    const urlSpec = urlSpecs.get(spec) ?? null;

    const builder = builders.get(spec.type);
    if (!builder) {
      console.warn(`[manifest] no builder registered for mark type "${spec.type}"`);
      continue;
    }

    const loaded = builder(spec);
    if (!loaded) continue;
    marks.push(loaded);

    /* If the mark has a URL source with a positive refreshInterval AND the
     * builder produced a `refresh` callback, schedule periodic re-fetches.
     * Each tick: try the real URL first; if it succeeds the builder gets
     * the real payload. If it fails and the health monitor has flagged
     * this mark offline, seed the spec with a fake payload so the chart
     * keeps moving — the matching DEMO HUD (wired in renderManifest)
     * tells the user the data isn't real. */
    if (urlSpec && urlSpec.refreshInterval > 0 && loaded.refresh) {
      const handle = setInterval(async () => {
        const ok = await fetchInto(spec, urlSpec!.url, urlSpec!.shape);
        health.recordFetch(spec.id, ok);
        if (ok) {
          loaded.refresh!(spec);
        } else if (health.stateOf(spec.id) === 'offline') {
          if (seedFakeIfPossible(spec, urlSpec!.shape)) loaded.refresh!(spec);
        }
      }, urlSpec.refreshInterval * 1000);
      intervals.push(handle);
    }
  }

  const dispose = () => {
    // Manifest-level refresh intervals (set up below for URL-data marks).
    for (const h of intervals) clearInterval(h);
    intervals.length = 0;

    // Walk every mark and ask its viz to tear itself down. Cells own work
    // the manifest layer can't see — splat-gallery has its own
    // setInterval(autoTimer) for photo cycling, liveImuCell drives its
    // own self-rescheduling poller, the spatial-audio cell holds a
    // looping AudioBufferSourceNode, the video panel owns an HLS
    // instance. Without this loop, every manifest replace (Vite HMR
    // during dev, leave-rejoin in XR) leaves the previous cells'
    // background work running and accumulates zombie timers — observed
    // as "SOG reloads every 5–10 seconds" because N stacked autoTimers
    // each fire at their own 15 s offset.
    for (const m of marks) {
      const v = m.viz as { dispose?: () => void } | null;
      if (v && typeof v.dispose === 'function') {
        try { v.dispose(); }
        catch (e) { console.warn(`[manifest] dispose failed for mark "${m.id}":`, e); }
      }
    }
    marks.length = 0;
  };

  return {
    name: manifest.name,
    scaleTag: manifest.scaleTag,
    marks,
    health,
    dispose,
  };
}

function shapeToField(shape: string, data: unknown): Record<string, unknown> {
  switch (shape) {
    case 'hierarchy': return { hierarchy: data };
    case 'graph': return { graph: data };
    case 'series': {
      // Accept either a bare array or a `{ samples: [...] }` wrapper —
      // the MagNET vitals device emits the wrapped form for /heart-rate/history,
      // /breathing/history, etc., to keep room for metadata fields later.
      if (data && typeof data === 'object' && Array.isArray((data as { samples?: unknown }).samples)) {
        return { series: (data as { samples: unknown[] }).samples };
      }
      return { series: data };
    }
    case 'distributions': return { distributions: data };
    case 'flow': return { flow: data };
    default: return {};
  }
}

/** Helper: extract hierarchy data from a MarkSpec's inline data. */
export function extractHierarchy(spec: MarkSpec): HNode | null {
  if (spec.data.source !== 'inline') return null;
  return (spec.data.hierarchy as HNode) ?? null;
}

/** Helper: extract graph data from a MarkSpec's inline data. */
export function extractGraph(spec: MarkSpec): { nodes: any[]; links: any[] } | null {
  if (spec.data.source !== 'inline') return null;
  return (spec.data as any).graph ?? null;
}

/** Helper: extract flow data from a MarkSpec's inline data. */
export function extractFlow(spec: MarkSpec): { nodes: any[]; links: any[] } | null {
  if (spec.data.source !== 'inline') return null;
  return (spec.data as any).flow ?? null;
}

/** Helper: extract series data from a MarkSpec's inline data. */
export function extractSeries(spec: MarkSpec): any[] | null {
  if (spec.data.source !== 'inline') return null;
  return (spec.data as any).series ?? null;
}

/** Helper: extract distributions data from a MarkSpec's inline data. */
export function extractDistributions(spec: MarkSpec): number[][] | null {
  if (spec.data.source !== 'inline') return null;
  return (spec.data as any).distributions ?? null;
}
