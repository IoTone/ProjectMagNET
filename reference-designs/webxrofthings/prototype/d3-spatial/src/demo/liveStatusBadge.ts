/**
 * Status badge for a live data cell.
 *
 * Visual contract (so future readers can predict what they'll see in the gallery):
 *   - `live`    → badge hidden entirely. The healthy case is the absence of chrome,
 *                 so the gallery doesn't get cluttered when everything is working.
 *   - `stale`   → small amber dot in the top-right corner. Says "we hit an error
 *                 but data is still recent" without forcing the user to read text.
 *   - `offline` → small red pill with "OFFLINE · 12s" (seconds since last success).
 *                 Reads as a status annotation rather than as the data going dark,
 *                 so the user can tell *why* the chart looks frozen.
 *
 * Anti-jitter is handled in `startPolling` (sticky offline flag + 2-success
 * recovery debounce). The badge just renders whatever state it's polled.
 *
 * Polling cadence is intentionally slow (default 500 ms) — the cells refresh
 * every 1–3 s and the user reads badges with their eyes, not their mind.
 */
import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { TEXT } from '../ui/palette';
import type { LiveCellState } from './liveVitalsCells';

export interface StatusBadgeOpts {
  /** Where to place the badge inside the cell's local frame. */
  position: THREE.Vector3;
  /** How often to re-read getStatus(). Default 500 ms. */
  pollMs?: number;
}

export interface StatusBadge {
  group: THREE.Group;
  /** Refresh visuals immediately from getStatus (also called by the timer). */
  refresh(): void;
  dispose(): void;
}

const DOT_RADIUS = 0.006;
const PILL_W = 0.07;
const PILL_H = 0.018;

/**
 * Format the offline pill text. Pure — exposed for unit testing without
 * pulling troika into the test env.
 *   formatOfflineLabel(null)   → 'OFFLINE'
 *   formatOfflineLabel(42_000) → 'OFFLINE · 42s'
 *   formatOfflineLabel(120000) → 'OFFLINE · 2m'
 *   formatOfflineLabel(154000) → 'OFFLINE · 2m 34s'
 */
export function formatOfflineLabel(lastSuccessAgoMs: number | null): string {
  if (lastSuccessAgoMs == null) return 'OFFLINE';
  const seconds = Math.floor(lastSuccessAgoMs / 1000);
  if (seconds < 60) return `OFFLINE · ${seconds}s`;
  const m = Math.floor(seconds / 60);
  const s = seconds % 60;
  return s ? `OFFLINE · ${m}m ${s}s` : `OFFLINE · ${m}m`;
}

export function buildStatusBadge(
  getStatus: () => { state: LiveCellState; lastSuccessAgoMs: number | null },
  opts: StatusBadgeOpts,
): StatusBadge {
  const { position, pollMs = 500 } = opts;

  const group = new THREE.Group();
  group.name = 'live-status-badge';
  group.position.copy(position);

  // Stale dot — single sphere, hidden in 'live'.
  const dotGeo = new THREE.SphereGeometry(DOT_RADIUS, 12, 8);
  const dotMat = new THREE.MeshBasicMaterial({ color: TEXT.warn, transparent: true, opacity: 0.95 });
  const dot = new THREE.Mesh(dotGeo, dotMat);
  dot.visible = false;
  group.add(dot);

  // Offline pill — small rounded rectangle (regular plane is fine at this size)
  // with a tiny "OFFLINE · 12s" text floating slightly forward.
  const pillGeo = new THREE.PlaneGeometry(PILL_W, PILL_H);
  const pillMat = new THREE.MeshBasicMaterial({ color: TEXT.error, transparent: true, opacity: 0.85 });
  const pill = new THREE.Mesh(pillGeo, pillMat);
  pill.visible = false;
  group.add(pill);

  const pillText = new Text();
  pillText.text = 'OFFLINE';
  pillText.fontSize = 0.008;
  pillText.color = 0x000000;            // black on the warm coral pill — high contrast
  pillText.anchorX = 'center';
  pillText.anchorY = 'middle';
  pillText.position.set(0, 0, 0.001);   // hair forward so it never z-fights the pill
  pillText.visible = false;
  pillText.sync();
  group.add(pillText);

  let lastRenderedKey = '';

  function refresh() {
    const { state, lastSuccessAgoMs } = getStatus();
    // Build a small render key — only re-sync the Text when content changes.
    // troika-three-text's sync() is the expensive part; this avoids syncing
    // every poll when nothing has changed.
    const seconds = lastSuccessAgoMs == null ? null : Math.floor(lastSuccessAgoMs / 1000);
    const key = state === 'offline' ? `offline:${seconds ?? 'never'}` : state;
    if (key === lastRenderedKey) return;
    lastRenderedKey = key;

    if (state === 'live') {
      dot.visible = false;
      pill.visible = false;
      pillText.visible = false;
      return;
    }
    if (state === 'stale') {
      dot.visible = true;
      pill.visible = false;
      pillText.visible = false;
      return;
    }
    // offline
    dot.visible = false;
    pill.visible = true;
    pillText.visible = true;
    pillText.text = formatOfflineLabel(lastSuccessAgoMs);
    pillText.sync();
  }

  refresh();
  const handle = setInterval(refresh, pollMs);

  return {
    group,
    refresh,
    dispose: () => {
      clearInterval(handle);
      dotGeo.dispose();
      dotMat.dispose();
      pillGeo.dispose();
      pillMat.dispose();
      // troika-three-text exposes dispose() on Text instances.
      (pillText as unknown as { dispose?: () => void }).dispose?.();
    },
  };
}
