/**
 * proxy-diag — observe a Vite proxy entry and expose request/error/latency stats.
 *
 * The vitals device firmware doesn't have a /healthz, and adding one means a
 * firmware flash. The proxy in front of it sees every request and response
 * anyway, so we tally there instead. Snapshot is exposed as JSON via a Connect
 * middleware mounted by vite.config.ts at e.g. /__diag/vitals.
 *
 * Usage:
 *   const diag = createProxyDiag('vitals');
 *   diag.attachToProxy(proxy);                // inside the proxy entry's `configure`
 *   server.middlewares.use('/__diag/vitals', diagMiddleware(diag));   // in a plugin
 *
 * The middleware also accepts POST /reset to clear counters between runs.
 */
import type { IncomingMessage, ServerResponse } from 'http';

export interface ProxyDiagSnapshot {
  name: string;
  uptime_s: number;
  in_flight: number;
  total: number;
  ok: number;
  err: number;
  drop_pct: number;
  by_status: Record<string, number>;
  by_endpoint: Record<string, EndpointStats>;
  latency_ms: { p50: number; p95: number; p99: number; max: number };
  recent_errors: Array<{ ts_iso: string; code: string; endpoint: string; ms: number }>;
  /** Scalar metrics extracted from proxied response bodies. Empty when no
   * extractors are registered for this diag instance. */
  values?: Record<string, ValueStats>;
}

export interface ValueStats {
  count: number;
  last: number;
  mean: number;
  min: number;
  max: number;
  p50: number;
  p95: number;
  window_s: number;
}

interface EndpointStats {
  total: number; ok: number; err: number; avg_ms: number; max_ms: number;
}

interface EndpointAccum {
  total: number; ok: number; err: number; sum_ms: number; max_ms: number;
}

/**
 * Per-URL extractor: parses a successful JSON response and returns 0+ named
 * scalar metrics. Returning `undefined`/`null`/non-finite for a key skips
 * that sample (so `/heart-rate` can return `{bpm: undefined}` when presence
 * is false, instead of polluting the average with zeros).
 */
export type ValueExtractor = (json: unknown) => Record<string, number | null | undefined>;
export type ValueExtractorRegistry = Record<string, ValueExtractor>;

export interface CreateDiagOptions {
  /** URL → extractor. Match is exact on `req.url` as the proxy sees it
   *  (i.e. with the original `/api/v1/...` prefix, not the rewritten path). */
  valueExtractors?: ValueExtractorRegistry;
  /** Rolling time window for value stats. Default 5 minutes. */
  valueWindowMs?: number;
  /** Body buffer cap to protect memory; responses larger than this are
   *  forwarded as normal but skipped for extraction. Default 64 KB. */
  valueBodyCapBytes?: number;
}

const RECENT_LAT_CAP = 200;
const RECENT_ERR_CAP = 50;
const DEFAULT_VALUE_WINDOW_MS = 5 * 60_000;
const DEFAULT_VALUE_BODY_CAP = 64 * 1024;

// Set DEBUG_PROXY_DIAG=1 to log per-response body extraction tracing on stderr.
const DEBUG = process.env.DEBUG_PROXY_DIAG === '1';

export interface ProxyDiag {
  attachToProxy(proxy: NodeJS.EventEmitter): void;
  snapshot(): ProxyDiagSnapshot;
  reset(): void;
  /** Test hook — feed events directly without an actual proxy. */
  _record(endpoint: string, ms: number, ok: boolean, statusKey: string): void;
  /** Test hook — feed extracted values directly without a body parse. */
  _recordValue(name: string, value: number, ts?: number): void;
}

export function createProxyDiag(name: string, opts: CreateDiagOptions = {}): ProxyDiag {
  const startedAt = Date.now();
  const valueExtractors = opts.valueExtractors ?? {};
  const valueWindowMs = opts.valueWindowMs ?? DEFAULT_VALUE_WINDOW_MS;
  const valueBodyCap = opts.valueBodyCapBytes ?? DEFAULT_VALUE_BODY_CAP;
  const hasExtractors = Object.keys(valueExtractors).length > 0;

  let total = 0, ok = 0, err = 0, inFlight = 0;
  const byStatus = new Map<string, number>();
  const byEndpoint = new Map<string, EndpointAccum>();
  const recentErrors: Array<{ ts: number; code: string; endpoint: string; ms: number }> = [];
  const recentLatencies: number[] = [];
  // metric name → time-windowed samples
  const valueSamples = new Map<string, Array<{ ts: number; v: number }>>();

  // Symbol attached to the incoming request so we can match proxyReq → proxyRes/error.
  const T0 = Symbol(`${name}-t0`);

  function record(endpoint: string, ms: number, ok_: boolean, statusKey: string) {
    total++;
    if (ok_) ok++; else err++;
    byStatus.set(statusKey, (byStatus.get(statusKey) ?? 0) + 1);
    let e = byEndpoint.get(endpoint);
    if (!e) { e = { total: 0, ok: 0, err: 0, sum_ms: 0, max_ms: 0 }; byEndpoint.set(endpoint, e); }
    e.total++;
    if (ok_) e.ok++; else e.err++;
    e.sum_ms += ms;
    if (ms > e.max_ms) e.max_ms = ms;
    recentLatencies.push(ms);
    if (recentLatencies.length > RECENT_LAT_CAP) recentLatencies.shift();
    if (!ok_) {
      recentErrors.push({ ts: Date.now(), code: statusKey, endpoint, ms });
      if (recentErrors.length > RECENT_ERR_CAP) recentErrors.shift();
    }
  }

  function recordValue(name: string, value: number, ts: number = Date.now()) {
    let buf = valueSamples.get(name);
    if (!buf) { buf = []; valueSamples.set(name, buf); }
    buf.push({ ts, v: value });
    // Cheap prune: only when buffer is moderately large, drop entries older
    // than the window. Avoids per-sample shifting on hot paths.
    if (buf.length > 32) {
      const cutoff = ts - valueWindowMs;
      while (buf.length > 0 && buf[0]!.ts < cutoff) buf.shift();
    }
  }

  function maybeExtractValues(url: string, body: string) {
    const ex = valueExtractors[url];
    if (!ex) return;
    let parsed: unknown;
    try { parsed = JSON.parse(body); }
    catch (e) { if (DEBUG) console.error(`[proxy-diag:${name}] JSON.parse failed for ${url}: ${(e as Error).message}`); return; }
    let metrics: Record<string, number | null | undefined>;
    try { metrics = ex(parsed); }
    catch (e) { if (DEBUG) console.error(`[proxy-diag:${name}] extractor threw for ${url}: ${(e as Error).message}`); return; }
    if (DEBUG) console.error(`[proxy-diag:${name}] extracted from ${url}:`, metrics);
    const now = Date.now();
    for (const [k, v] of Object.entries(metrics)) {
      if (v != null && Number.isFinite(v)) recordValue(k, v as number, now);
    }
  }

  function attachToProxy(proxy: NodeJS.EventEmitter) {
    proxy.on('proxyReq', (_proxyReq: unknown, req: IncomingMessage) => {
      inFlight++;
      (req as unknown as Record<symbol, number>)[T0] = Date.now();
    });
    proxy.on('proxyRes', (proxyRes: { statusCode?: number } & NodeJS.ReadableStream, req: IncomingMessage) => {
      inFlight = Math.max(0, inFlight - 1);
      const t0 = (req as unknown as Record<symbol, number>)[T0] ?? Date.now();
      const dt = Date.now() - t0;
      const code = String(proxyRes.statusCode ?? 'unknown');
      const ok_ = proxyRes.statusCode != null && proxyRes.statusCode >= 200 && proxyRes.statusCode < 400;
      record(req.url ?? '?', dt, ok_, code);

      // Side-channel body buffering for value extraction. Attaching listeners
      // here (synchronously, before http-proxy's pipe call) is safe: both our
      // listeners and the pipe receive the same data events on the flowing
      // stream. We cap total bytes to protect against runaway buffers.
      const url = req.url ?? '';
      const ex = valueExtractors[url];
      if (DEBUG) console.error(`[proxy-diag:${name}] proxyRes ${url} status=${code} hasExtractor=${!!ex} ok=${ok_}`);
      if (hasExtractors && ok_ && ex) {
        const chunks: Buffer[] = [];
        let n = 0;
        let truncated = false;
        proxyRes.on('data', (chunk: Buffer) => {
          if (truncated) return;
          if (n + chunk.length > valueBodyCap) { truncated = true; return; }
          chunks.push(chunk);
          n += chunk.length;
        });
        proxyRes.on('end', () => {
          if (truncated) {
            if (DEBUG) console.error(`[proxy-diag:${name}] body truncated for ${url} (${n} bytes)`);
            return;
          }
          const body = Buffer.concat(chunks).toString('utf8');
          if (DEBUG) console.error(`[proxy-diag:${name}] body for ${url} (${body.length}b): ${body.slice(0, 120)}`);
          maybeExtractValues(url, body);
        });
      }
    });
    proxy.on('error', (e: NodeJS.ErrnoException, req: IncomingMessage) => {
      inFlight = Math.max(0, inFlight - 1);
      const t0 = (req as unknown as Record<symbol, number>)[T0] ?? Date.now();
      const dt = Date.now() - t0;
      const code = e?.code ?? 'EUNKNOWN';
      record(req.url ?? '?', dt, false, code);
    });
  }

  function percentile(sorted: number[], q: number): number {
    if (sorted.length === 0) return 0;
    const idx = Math.min(sorted.length - 1, Math.ceil((sorted.length * q) / 100) - 1);
    return sorted[Math.max(0, idx)] ?? 0;
  }

  function valueStatsSnapshot(now: number): Record<string, ValueStats> {
    const out: Record<string, ValueStats> = {};
    const cutoff = now - valueWindowMs;
    for (const [name, buf] of valueSamples) {
      // Window-prune at snapshot time so reads never lie even if no recent writes.
      while (buf.length > 0 && buf[0]!.ts < cutoff) buf.shift();
      if (buf.length === 0) continue;
      let sum = 0, mn = Infinity, mx = -Infinity;
      const sorted = new Array<number>(buf.length);
      for (let i = 0; i < buf.length; i++) {
        const v = buf[i]!.v;
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sorted[i] = v;
      }
      sorted.sort((a, b) => a - b);
      out[name] = {
        count: buf.length,
        last: buf[buf.length - 1]!.v,
        mean: round1(sum / buf.length),
        min: round1(mn),
        max: round1(mx),
        p50: round1(percentile(sorted, 50)),
        p95: round1(percentile(sorted, 95)),
        window_s: Math.round((now - buf[0]!.ts) / 1000),
      };
    }
    return out;
  }

  function snapshot(): ProxyDiagSnapshot {
    const now = Date.now();
    const sorted = [...recentLatencies].sort((a, b) => a - b);
    const snap: ProxyDiagSnapshot = {
      name,
      uptime_s: Math.floor((now - startedAt) / 1000),
      in_flight: inFlight,
      total, ok, err,
      drop_pct: total === 0 ? 0 : Math.round((err / total) * 1000) / 10,
      by_status: Object.fromEntries(byStatus),
      by_endpoint: Object.fromEntries(
        [...byEndpoint].map(([k, v]): [string, EndpointStats] => [k, {
          total: v.total, ok: v.ok, err: v.err,
          avg_ms: v.total ? Math.round(v.sum_ms / v.total) : 0,
          max_ms: v.max_ms,
        }])
      ),
      latency_ms: {
        p50: percentile(sorted, 50),
        p95: percentile(sorted, 95),
        p99: percentile(sorted, 99),
        max: sorted.length === 0 ? 0 : sorted[sorted.length - 1] ?? 0,
      },
      recent_errors: recentErrors.map(e => ({
        ts_iso: new Date(e.ts).toISOString(),
        code: e.code, endpoint: e.endpoint, ms: e.ms,
      })),
    };
    // Include `values` when this diag is configured to extract them
    // (so "we're listening, just no data yet" is visible) or when any
    // samples exist (so external feeds via _recordValue are visible too).
    if (hasExtractors || valueSamples.size > 0) snap.values = valueStatsSnapshot(now);
    return snap;
  }

  function reset() {
    total = 0; ok = 0; err = 0; inFlight = 0;
    byStatus.clear(); byEndpoint.clear();
    recentErrors.length = 0; recentLatencies.length = 0;
    valueSamples.clear();
  }

  return {
    attachToProxy, snapshot, reset,
    _record: record,
    _recordValue: recordValue,
  };
}

function round1(n: number): number { return Math.round(n * 10) / 10; }

/**
 * Connect-style middleware. Mount under a path (e.g. `/__diag/vitals`); req.url
 * inside the handler is stripped to the suffix, so '/' returns the snapshot
 * and '/reset' (POST) clears counters.
 */
export function diagMiddleware(diag: ProxyDiag) {
  return (req: IncomingMessage, res: ServerResponse, next?: (err?: unknown) => void) => {
    try {
      if (req.method === 'POST' && (req.url === '/reset' || req.url === '/reset/')) {
        diag.reset();
        res.writeHead(204);
        res.end();
        return;
      }
      const body = JSON.stringify(diag.snapshot(), null, 2);
      res.setHeader('content-type', 'application/json; charset=utf-8');
      res.setHeader('cache-control', 'no-store');
      res.end(body);
    } catch (e) {
      if (next) next(e); else { res.statusCode = 500; res.end(String(e)); }
    }
  };
}
