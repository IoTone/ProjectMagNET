/**
 * Shared polling helper for "live cell" modules.
 *
 * Self-rescheduling poller with exponential backoff + status hysteresis.
 * Extracted from `liveVitalsCells.ts` so the IMU / camera / future cells
 * can reuse the same offline-detection behaviour (the badge UI in
 * `liveStatusBadge.ts` consumes whatever shape `getStatus()` returns,
 * so anyone built on this gets the same offline/stale/live indication
 * for free).
 *
 * Why not setInterval: ESP-IDF's esp_http_server has only 7 listen sockets
 * by default and doesn't keep-alive cleanly through Vite's proxy. With
 * setInterval, a slow or failing device piles up overlapping fetches and
 * eventually starves its own httpd or watchdogs out. Self-rescheduling
 * setTimeout naturally serializes — the next tick can't fire until the
 * previous fetch resolves — and lets us back off when the device errors
 * so we give it room to recover instead of hammering it on the way down.
 *
 * `refresh` should resolve to `true` when the request succeeded (or returned
 * legitimately empty data — that's not a device fault) and `false` when the
 * device returned a non-2xx or threw. Backoff doubles per consecutive error,
 * capped at MAX_BACKOFF, and resets on the first success.
 */

export type LiveCellState = 'live' | 'stale' | 'offline';

export interface LiveCellStatus {
  state: LiveCellState;
  /** ms since last successful fetch, or null if never. */
  lastSuccessAgoMs: number | null;
}

export interface PollingHandle {
  stop(): void;
  getStatus(): LiveCellStatus;
}

export const MAX_BACKOFF_MS = 30_000;
export const OFFLINE_AFTER_REFRESH_MULTIPLES = 6;
export const RECOVERY_SUCCESS_COUNT = 2;
/**
 * Initial-failure offline threshold. Two failed attempts is enough evidence
 * that the device isn't reachable when we've never had a success — the
 * common "device powered off, browser fires polls anyway" case. Was 3 (~45 s
 * to flip the badge with vite's 15 s proxy timeout); 2 brings that to ~30 s
 * which feels responsive without being trigger-happy.
 */
export const INITIAL_OFFLINE_AFTER_ERRORS = 2;

export function startPolling(
  refreshMs: number,
  refresh: () => Promise<boolean>,
): PollingHandle {
  let errs = 0;
  let consecutiveSuccesses = 0;
  let lastSuccessTs: number | null = null;
  // Sticky offline flag — once we transition to offline we stay there until
  // RECOVERY_SUCCESS_COUNT successes land, even if the latest fetch happened
  // to succeed. Prevents single-success flaps from clearing a real outage.
  let stickyOffline = false;
  let cancelled = false;
  let handle: ReturnType<typeof setTimeout> | null = null;

  const offlineWindowMs = refreshMs * OFFLINE_AFTER_REFRESH_MULTIPLES;

  const tick = async () => {
    if (cancelled) return;
    let ok = false;
    try { ok = await refresh(); } catch { /* defensive — refresh swallows its own */ }
    if (cancelled) return;
    if (ok) {
      errs = 0;
      consecutiveSuccesses++;
      lastSuccessTs = Date.now();
      if (stickyOffline && consecutiveSuccesses >= RECOVERY_SUCCESS_COUNT) {
        stickyOffline = false;
      }
    } else {
      errs++;
      consecutiveSuccesses = 0;
      // Latch the offline flag the moment we cross either threshold so the
      // status reads correctly between ticks. Two distinct entry conditions:
      //   - never connected (lastSuccessTs null) and 2+ tries failed
      //   - had a success previously, but it's now stale beyond the window
      // Important: the time-based check requires lastSuccessTs to exist —
      // otherwise we'd flip offline on the first error before ever connecting.
      if (lastSuccessTs == null && errs >= INITIAL_OFFLINE_AFTER_ERRORS) {
        stickyOffline = true;
      } else if (lastSuccessTs != null && (Date.now() - lastSuccessTs) > offlineWindowMs) {
        stickyOffline = true;
      }
    }
    const delay = errs > 0
      ? Math.min(refreshMs * (1 << Math.min(errs - 1, 5)), MAX_BACKOFF_MS)
      : refreshMs;
    handle = setTimeout(tick, delay);
  };

  function getStatus(): LiveCellStatus {
    const now = Date.now();
    const lastSuccessAgoMs = lastSuccessTs == null ? null : now - lastSuccessTs;
    // Time-based offline check is recomputed on every read — handles the case
    // where ticks have stopped firing (e.g. backoff is long) but the silence
    // itself should already register as offline.
    const timeOffline = lastSuccessTs != null && lastSuccessAgoMs! > offlineWindowMs;
    const initialOffline = lastSuccessTs == null && errs >= INITIAL_OFFLINE_AFTER_ERRORS;
    if (stickyOffline || timeOffline || initialOffline) {
      return { state: 'offline', lastSuccessAgoMs };
    }
    if (errs >= 1) return { state: 'stale', lastSuccessAgoMs };
    return { state: 'live', lastSuccessAgoMs };
  }

  // Fire immediately so the first paint isn't gated on a refreshMs delay;
  // matches the prior setInterval behavior where refresh() ran synchronously.
  void tick();

  return {
    stop() { cancelled = true; if (handle) clearTimeout(handle); },
    getStatus,
  };
}
