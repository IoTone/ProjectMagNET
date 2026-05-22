/**
 * healthMonitor — per-mark fetch health tracking for the manifest layer.
 *
 * The per-cell `liveVitalsCells` already has `startPolling` + a
 * stale/offline state machine, but the *manifest-driven* marks (UC1
 * vitals, UC2 camera + env, etc.) go through `loader.ts`'s setInterval
 * loop which has no equivalent. That meant a dead sensor produced "the
 * chart just stops moving" with no UI cue.
 *
 * This monitor sits at the loader's seam: every fetchInto call reports
 * success or failure, and the monitor maintains a per-mark state. The
 * dataspace's offline-sensors HUD subscribes to state changes and
 * relabels itself; the loader inspects state to decide whether to feed
 * fake data into the refresh callback.
 *
 * Hysteresis matches the per-cell rules so users see consistent
 * behaviour across the gallery-and-manifest divide:
 *   - `live`    : initial state and after 2+ consecutive successes.
 *   - `stale`   : 1 consecutive failure but a recent success.
 *   - `offline` : 3+ consecutive failures, OR last success older than
 *                 6× the refresh interval.
 * Recovery from `offline` requires 2 consecutive successes (so a
 * single flap doesn't drop the DEMO indicator and pull the rug out
 * from under a user who's reading the fake data).
 */

export type MarkHealth = 'live' | 'stale' | 'offline';

interface MarkState {
  failures: number;
  successesSinceOffline: number;
  lastSuccessMs: number | null;
  state: MarkHealth;
  refreshMs: number;
}

const OFFLINE_AFTER_FAILS         = 3;
/* Lower threshold while we've never seen a single successful fetch.
 * Matches livePolling's INITIAL_OFFLINE_AFTER_ERRORS rule: if a
 * sensor is dead at session start, surface that in the UI instantly
 * (1 failure → offline → fake data + HUDs) rather than making the
 * user wait 3 refresh cycles to see anything happen. */
const INITIAL_OFFLINE_AFTER_FAILS = 1;
const RECOVERY_SUCCESS_COUNT      = 2;
const STALE_WINDOW_MULTIPLE       = 6;

export interface HealthMonitor {
  /** Register a mark with its expected refresh cadence (seconds). */
  register(markId: string, refreshSeconds: number): void;
  /** Record one fetch attempt's outcome. */
  recordFetch(markId: string, success: boolean): void;
  /** Lookup current state. Returns 'live' for unknown ids — calling code
   *  that doesn't care about health (inline-data marks, etc.) is silently
   *  treated as healthy rather than forcing it to register. */
  stateOf(markId: string): MarkHealth;
  /** Forcibly set a mark's state, bypassing the failure-count hysteresis.
   *  Used by the manifest layer to mirror state from self-fetching cells
   *  (imu, video) whose own pollers track health internally — the cell
   *  reports its own state via getStatus() and the manifest layer
   *  reflects that into the global health monitor so the DEMO HUD wires
   *  up uniformly. No-op for unregistered ids. */
  forceState(markId: string, state: MarkHealth): void;
  /** All currently-offline mark ids. Sorted for stable HUD rendering. */
  offlineMarkIds(): string[];
  /** Subscribe to state-change events. The callback fires whenever the
   *  set of offline marks changes (one going on or coming off). Returns
   *  an unsubscribe function. */
  onChange(cb: (offlineIds: string[]) => void): () => void;
}

export function createHealthMonitor(): HealthMonitor {
  const states = new Map<string, MarkState>();
  const listeners = new Set<(offlineIds: string[]) => void>();
  let lastOfflineSnapshot = '';

  function offlineMarkIds(): string[] {
    const out: string[] = [];
    for (const [id, s] of states) if (s.state === 'offline') out.push(id);
    return out.sort();
  }

  function fire() {
    const next = offlineMarkIds();
    const key = next.join('|');
    if (key === lastOfflineSnapshot) return;
    lastOfflineSnapshot = key;
    for (const cb of listeners) {
      try { cb(next); } catch (e) { console.warn('[healthMonitor] listener threw:', e); }
    }
  }

  return {
    register(markId, refreshSeconds) {
      if (states.has(markId)) return;
      states.set(markId, {
        failures: 0,
        successesSinceOffline: 0,
        lastSuccessMs: null,
        state: 'live',
        refreshMs: Math.max(1000, refreshSeconds * 1000),
      });
    },

    recordFetch(markId, success) {
      const s = states.get(markId);
      if (!s) return;
      const now = Date.now();

      if (success) {
        s.failures = 0;
        s.lastSuccessMs = now;
        if (s.state === 'offline') {
          s.successesSinceOffline++;
          if (s.successesSinceOffline >= RECOVERY_SUCCESS_COUNT) {
            s.state = 'live';
            s.successesSinceOffline = 0;
          }
          /* Otherwise stay in `offline` until the second success — the
           * single-success window keeps a flapping endpoint from yanking
           * the fake-data display in and out. */
        } else {
          s.state = 'live';
          s.successesSinceOffline = 0;
        }
      } else {
        s.failures++;
        s.successesSinceOffline = 0;
        const neverSucceeded = s.lastSuccessMs == null;
        const threshold      = neverSucceeded ? INITIAL_OFFLINE_AFTER_FAILS : OFFLINE_AFTER_FAILS;
        const staleAge       = s.refreshMs * STALE_WINDOW_MULTIPLE;
        const tooStale       = s.lastSuccessMs != null && (now - s.lastSuccessMs) > staleAge;
        if (s.failures >= threshold || tooStale) {
          s.state = 'offline';
        } else if (s.failures >= 1) {
          s.state = 'stale';
        }
      }

      fire();
    },

    stateOf(markId) {
      return states.get(markId)?.state ?? 'live';
    },

    forceState(markId, state) {
      const s = states.get(markId);
      if (!s) return;
      if (s.state === state) return;
      s.state = state;
      if (state === 'live') {
        s.failures = 0;
        s.lastSuccessMs = Date.now();
      }
      fire();
    },

    offlineMarkIds,

    onChange(cb) {
      listeners.add(cb);
      return () => listeners.delete(cb);
    },
  };
}
