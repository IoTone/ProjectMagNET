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

/** Load a manifest and instantiate all its marks. */
export async function loadManifest(manifest: DataspaceManifest, token?: string): Promise<LoadResult> {
  const marks: LoadedMark[] = [];
  const intervals: Array<ReturnType<typeof setInterval>> = [];

  /** Re-fetch a URL data source and mutate spec.data with the new inline data.
   *  Returns true on success. */
  async function fetchInto(spec: MarkSpec, url: string, shape: string): Promise<boolean> {
    try {
      const headers: Record<string, string> = {};
      if (token) headers['Authorization'] = `Bearer ${token}`;
      const resp = await fetch(url, { headers });
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
    }
  }

  for (const spec of manifest.marks) {
    let urlSpec: { url: string; shape: string; refreshInterval: number } | null = null;

    if (spec.data.source === 'url') {
      const urlData = spec.data;
      const url = urlData.url;

      // Handle WebSocket data sources (wss:// or ws://)
      if (url.startsWith('wss://') || url.startsWith('ws://')) {
        console.warn(`[manifest] skipping WebSocket source ${url} (not yet connected)`);
        continue;
      }

      urlSpec = { url, shape: urlData.shape, refreshInterval: urlData.refreshInterval ?? 0 };

      // Builders that own their own URL fetch (binary streams, opaque
      // formats — hls.js for video, spark for splat-gallery) need the URL
      // passed through `spec.data.url` unchanged, NOT pre-fetched and
      // parsed as JSON. Skipping the pre-fetch here keeps those builders
      // wired to the manifest's URL while still recording urlSpec so the
      // refresh-interval scheduler below remains correct for any future
      // builder that wants it.
      const SELF_FETCHING_SHAPES = new Set(['video', 'imu']);
      if (SELF_FETCHING_SHAPES.has(urlData.shape)) {
        // Skip the pre-fetch — the builder will consume the URL directly.
      } else {
        const ok = await fetchInto(spec, url, urlData.shape);
        if (!ok) continue;
      }
    }

    const builder = builders.get(spec.type);
    if (!builder) {
      console.warn(`[manifest] no builder registered for mark type "${spec.type}"`);
      continue;
    }

    const loaded = builder(spec);
    if (!loaded) continue;
    marks.push(loaded);

    /* If the mark has a URL source with a positive refreshInterval AND the
     * builder produced a `refresh` callback, schedule periodic re-fetches. */
    if (urlSpec && urlSpec.refreshInterval > 0 && loaded.refresh) {
      const handle = setInterval(async () => {
        const ok = await fetchInto(spec, urlSpec!.url, urlSpec!.shape);
        if (ok) loaded.refresh!(spec);
      }, urlSpec.refreshInterval * 1000);
      intervals.push(handle);
    }
  }

  const dispose = () => { for (const h of intervals) clearInterval(h); intervals.length = 0; };

  return {
    name: manifest.name,
    scaleTag: manifest.scaleTag,
    marks,
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
