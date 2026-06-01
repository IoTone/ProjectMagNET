/**
 * manifestController — owns the lifecycle of "the dataspace manifest the
 * user is currently viewing." Lifted out of main.ts so the orchestrator
 * doesn't repeat the dispose+load+render dance at every entry point.
 *
 * Two load paths share most of the work:
 *
 *   - `loadFromUrl(url)`  — the `?manifest=…` direct path; fetches itself.
 *   - `loadFromObject(m)` — the join-flow path; caller has already fetched
 *                           and parsed (because it needs an auth token).
 *
 * On every load: dispose the previous result first (stops its refresh
 * intervals), then call the caller-supplied `render`, then notify
 * `onLoaded`, then re-skin the privacy banner. On failure, calls
 * `onError` (typically: fall back to the demo gallery) and leaves
 * `currentResult` untouched.
 *
 * `dispose()` clears the active manifest's intervals — call from
 * `leave-dataspace` and on `pagehide` / HMR cleanup.
 */

import { loadManifest, type LoadResult } from './loader';
import type { DataspaceManifest } from './schema';
import type { PrivacyBannerController } from '../ui/privacyBannerController';

export interface ManifestController {
  loadFromUrl(url: string, token?: string): Promise<void>;
  loadFromObject(manifest: DataspaceManifest, token?: string): Promise<void>;
  /** Stop the current manifest's refresh intervals; clear it. No-op if none loaded. */
  dispose(): void;
  /** True when a manifest is currently loaded. */
  hasActive(): boolean;
}

export interface ManifestControllerOptions {
  /** Render the just-loaded marks into the scene. Caller controls vizAnchor
   *  positioning, group attach, etc. */
  render: (result: LoadResult, manifest: DataspaceManifest) => void;
  /** Privacy banner — re-skinned on every successful load; auto-shows when
   *  the manifest tags a device with `health-data`. */
  privacyBanner: PrivacyBannerController;
  /** Notification after a successful load — typically wires the dataspace
   *  HUD menu from `manifest.hud`. */
  onLoaded?: (manifest: DataspaceManifest, result: LoadResult) => void;
  /** Notification on load failure (URL fetch, JSON parse, or builder error). */
  onError?: (err: unknown, source: 'url' | 'object') => void;
}

export function createManifestController(opts: ManifestControllerOptions): ManifestController {
  const { render, privacyBanner, onLoaded, onError } = opts;
  let current: LoadResult | null = null;

  function dispose() {
    if (current) {
      current.dispose();
      current = null;
    }
  }

  async function ingest(manifest: DataspaceManifest, token: string | undefined, source: 'url' | 'object') {
    // Breadcrumbs flow to the in-headset DebugConsole — on Spectacles
    // (no chrome://inspect) the last one printed before a freeze tells
    // us whether the failure is in manifest parse, mark building, scene
    // render, or the onLoaded HUD wiring.
    try {
      console.info(`[manifest] ingest "${manifest.name}" (${source}) — disposing previous`);
      dispose();
      console.info(`[manifest] loadManifest: building ${manifest.marks.length} marks …`);
      const result = await loadManifest(manifest, token);
      current = result;
      console.info(`[manifest] built ${result.marks.length} marks — rendering to scene …`);
      render(result, manifest);
      console.info('[manifest] render done — wiring HUD (onLoaded) …');
      onLoaded?.(manifest, result);
      privacyBanner.attach(manifest, /* autoShow */ true);
      console.info(`[manifest] ingest complete ✓ "${manifest.name}"`);
    } catch (e) {
      console.error('[manifestController] load failed:', e);
      onError?.(e, source);
    }
  }

  return {
    async loadFromUrl(url, token) {
      try {
        const headers: Record<string, string> = {};
        if (token) headers['Authorization'] = `Bearer ${token}`;
        const resp = await fetch(url, { headers });
        if (!resp.ok) {
          const err = new Error(`${url} → ${resp.status}`);
          console.error('[manifestController] fetch failed:', err);
          onError?.(err, 'url');
          return;
        }
        const manifest = await resp.json() as DataspaceManifest;
        await ingest(manifest, token, 'url');
      } catch (e) {
        console.error('[manifestController] fetch threw:', e);
        onError?.(e, 'url');
      }
    },
    loadFromObject(manifest, token) {
      return ingest(manifest, token, 'object');
    },
    dispose,
    hasActive() {
      return current !== null;
    },
  };
}
