import { describe, it, expect, vi } from 'vitest';
import { EventEmitter } from 'events';
import { IncomingMessage, ServerResponse } from 'http';
import { Socket } from 'net';
import { createProxyDiag, diagMiddleware } from './proxy-diag';

/* ─── helpers ───────────────────────────────────────────────────────── */

function fakeReq(url: string, method = 'GET'): IncomingMessage {
  const r = new IncomingMessage(new Socket());
  r.url = url;
  r.method = method;
  return r;
}

function fakeRes(): ServerResponse & { _written: string; _status: number; _headers: Record<string, string> } {
  const r: any = new EventEmitter();
  r._written = '';
  r._status = 200;
  r._headers = {};
  r.writeHead = (code: number) => { r._status = code; return r; };
  r.setHeader = (k: string, v: string) => { r._headers[k.toLowerCase()] = v; };
  r.end = (chunk?: string) => { if (chunk) r._written += chunk; };
  r.statusCode = 200;
  return r;
}

/* ─── collector via the proxy event surface ─────────────────────────── */

describe('createProxyDiag — proxy event hookup', () => {
  it('records a successful proxyReq → proxyRes round trip', async () => {
    const diag = createProxyDiag('vitals');
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);

    const req = fakeReq('/heart-rate');
    proxy.emit('proxyReq', {}, req);
    // Tiny wait so latency is non-zero in the snapshot.
    await new Promise(r => setTimeout(r, 5));
    proxy.emit('proxyRes', { statusCode: 200 }, req);

    const snap = diag.snapshot();
    expect(snap.total).toBe(1);
    expect(snap.ok).toBe(1);
    expect(snap.err).toBe(0);
    expect(snap.in_flight).toBe(0);
    expect(snap.by_status['200']).toBe(1);
    expect(snap.by_endpoint['/heart-rate']?.total).toBe(1);
    expect(snap.by_endpoint['/heart-rate']?.ok).toBe(1);
    expect(snap.latency_ms.max).toBeGreaterThanOrEqual(0);
  });

  it('records a 5xx as a failure (separate from network errors)', () => {
    const diag = createProxyDiag('vitals');
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);

    const req = fakeReq('/targets');
    proxy.emit('proxyReq', {}, req);
    proxy.emit('proxyRes', { statusCode: 502 }, req);

    const snap = diag.snapshot();
    expect(snap.ok).toBe(0);
    expect(snap.err).toBe(1);
    expect(snap.by_status['502']).toBe(1);
    expect(snap.recent_errors[0]).toMatchObject({ code: '502', endpoint: '/targets' });
  });

  it('records proxy network errors with errno code', () => {
    const diag = createProxyDiag('vitals');
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);

    const req = fakeReq('/breathing');
    proxy.emit('proxyReq', {}, req);
    const err = new Error('refused') as NodeJS.ErrnoException;
    err.code = 'ECONNREFUSED';
    proxy.emit('error', err, req);

    const snap = diag.snapshot();
    expect(snap.err).toBe(1);
    expect(snap.by_status['ECONNREFUSED']).toBe(1);
    expect(snap.recent_errors[0]?.code).toBe('ECONNREFUSED');
  });

  it('tracks in-flight count between proxyReq and proxyRes', () => {
    const diag = createProxyDiag('vitals');
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);

    const r1 = fakeReq('/a');
    const r2 = fakeReq('/b');
    proxy.emit('proxyReq', {}, r1);
    proxy.emit('proxyReq', {}, r2);
    expect(diag.snapshot().in_flight).toBe(2);

    proxy.emit('proxyRes', { statusCode: 200 }, r1);
    expect(diag.snapshot().in_flight).toBe(1);

    proxy.emit('proxyRes', { statusCode: 200 }, r2);
    expect(diag.snapshot().in_flight).toBe(0);
  });

  it('drop_pct reflects (err / total) × 100, rounded to 1 decimal', () => {
    const diag = createProxyDiag('x');
    // 7 ok + 3 err = 30.0%
    for (let i = 0; i < 7; i++) diag._record('/a', 10, true, '200');
    for (let i = 0; i < 3; i++) diag._record('/a', 10, false, '500');
    expect(diag.snapshot().drop_pct).toBe(30);
  });
});

/* ─── snapshot derivations ─────────────────────────────────────────── */

describe('snapshot()', () => {
  it('computes per-endpoint avg/max ms', () => {
    const diag = createProxyDiag('x');
    diag._record('/a', 10, true, '200');
    diag._record('/a', 30, true, '200');
    diag._record('/b', 100, true, '200');

    const snap = diag.snapshot();
    expect(snap.by_endpoint['/a']?.avg_ms).toBe(20);
    expect(snap.by_endpoint['/a']?.max_ms).toBe(30);
    expect(snap.by_endpoint['/b']?.avg_ms).toBe(100);
    expect(snap.by_endpoint['/b']?.max_ms).toBe(100);
  });

  it('computes p50/p95/p99/max from the latency ring buffer', () => {
    const diag = createProxyDiag('x');
    // 100 samples 1..100 → p50≈50, p95≈95, p99≈99, max=100
    for (let i = 1; i <= 100; i++) diag._record('/a', i, true, '200');
    const { latency_ms: l } = diag.snapshot();
    expect(l.max).toBe(100);
    expect(l.p50).toBeGreaterThanOrEqual(49);
    expect(l.p50).toBeLessThanOrEqual(51);
    expect(l.p95).toBeGreaterThanOrEqual(94);
    expect(l.p95).toBeLessThanOrEqual(96);
    expect(l.p99).toBeGreaterThanOrEqual(98);
    expect(l.p99).toBeLessThanOrEqual(100);
  });

  it('caps the latency ring buffer at 200 samples', () => {
    const diag = createProxyDiag('x');
    for (let i = 1; i <= 500; i++) diag._record('/a', i, true, '200');
    // Newest 200 samples kept (301..500), so p50 should be ~400.
    const { latency_ms: l } = diag.snapshot();
    expect(l.p50).toBeGreaterThanOrEqual(395);
    expect(l.p50).toBeLessThanOrEqual(405);
  });

  it('caps recent_errors at 50', () => {
    const diag = createProxyDiag('x');
    for (let i = 0; i < 100; i++) diag._record(`/e${i}`, 1, false, '500');
    const snap = diag.snapshot();
    expect(snap.recent_errors.length).toBe(50);
    // Most-recent end is preserved.
    expect(snap.recent_errors[snap.recent_errors.length - 1]?.endpoint).toBe('/e99');
  });

  it('handles the no-traffic case gracefully', () => {
    const diag = createProxyDiag('x');
    const snap = diag.snapshot();
    expect(snap.total).toBe(0);
    expect(snap.drop_pct).toBe(0);
    expect(snap.latency_ms).toEqual({ p50: 0, p95: 0, p99: 0, max: 0 });
    expect(snap.by_endpoint).toEqual({});
  });
});

/* ─── reset ────────────────────────────────────────────────────────── */

describe('reset()', () => {
  it('clears all counters and buffers', () => {
    const diag = createProxyDiag('x');
    diag._record('/a', 10, true, '200');
    diag._record('/a', 10, false, '500');
    diag.reset();
    const snap = diag.snapshot();
    expect(snap.total).toBe(0);
    expect(snap.err).toBe(0);
    expect(snap.by_endpoint).toEqual({});
    expect(snap.recent_errors).toEqual([]);
  });
});

/* ─── value extraction ─────────────────────────────────────────────── */

describe('value extraction from response bodies', () => {
  /** Drive a proxyRes synchronously, then deliver fake body chunks. */
  function emitResponseWithBody(
    proxy: EventEmitter,
    url: string,
    body: string,
    statusCode = 200,
  ) {
    const req = fakeReq(url);
    proxy.emit('proxyReq', {}, req);
    // proxyRes must look like a Readable enough to attach data/end listeners.
    const proxyRes = new EventEmitter() as EventEmitter & { statusCode?: number };
    proxyRes.statusCode = statusCode;
    proxy.emit('proxyRes', proxyRes, req);
    proxyRes.emit('data', Buffer.from(body, 'utf8'));
    proxyRes.emit('end');
  }

  it('extracts a single scalar and reports stats', () => {
    const diag = createProxyDiag('vitals', {
      valueExtractors: {
        '/heart-rate': (j: any) => ({ bpm: typeof j?.bpm === 'number' ? j.bpm : undefined }),
      },
    });
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);

    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ bpm: 70 }));
    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ bpm: 80 }));
    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ bpm: 60 }));

    const snap = diag.snapshot();
    expect(snap.values?.bpm).toMatchObject({ count: 3, last: 60, mean: 70, min: 60, max: 80 });
  });

  it('skips samples when extractor returns undefined (e.g. presence false)', () => {
    const diag = createProxyDiag('vitals', {
      valueExtractors: {
        '/heart-rate': (j: any) =>
          ({ bpm: j?.presence ? j.bpm : undefined }),
      },
    });
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);

    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ presence: true,  bpm: 72 }));
    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ presence: false, bpm: 0  })); // skipped
    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ presence: true,  bpm: 78 }));

    const snap = diag.snapshot();
    expect(snap.values?.bpm?.count).toBe(2);
    expect(snap.values?.bpm?.mean).toBe(75);
  });

  it('extracts multiple metrics from one response', () => {
    const diag = createProxyDiag('vitals', {
      valueExtractors: {
        '/targets': (j: any) => ({
          target_count: j?.count,
          nearest_m: Array.isArray(j?.targets) && j.targets[0]
            ? Math.hypot(j.targets[0].x_m, j.targets[0].y_m) : undefined,
        }),
      },
    });
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);

    emitResponseWithBody(proxy, '/targets', JSON.stringify({
      count: 2, targets: [{ x_m: 0, y_m: 1 }, { x_m: 0, y_m: 2 }],
    }));

    const snap = diag.snapshot();
    expect(snap.values?.target_count?.last).toBe(2);
    expect(snap.values?.nearest_m?.last).toBe(1);
  });

  it('does not extract from endpoints with no extractor registered', () => {
    const diag = createProxyDiag('vitals', {
      valueExtractors: { '/heart-rate': (j: any) => ({ bpm: j?.bpm }) },
    });
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);

    emitResponseWithBody(proxy, '/breathing', JSON.stringify({ rpm: 16 }));
    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ bpm: 70 }));

    const snap = diag.snapshot();
    expect(Object.keys(snap.values ?? {})).toEqual(['bpm']);
  });

  it('skips extraction on non-2xx', () => {
    const diag = createProxyDiag('vitals', {
      valueExtractors: { '/heart-rate': (j: any) => ({ bpm: j?.bpm }) },
    });
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);
    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ bpm: 70 }), 500);
    expect(diag.snapshot().values).toEqual({});
  });

  it('truncates extraction when body exceeds the cap', () => {
    const diag = createProxyDiag('vitals', {
      valueExtractors: { '/heart-rate': (j: any) => ({ bpm: j?.bpm }) },
      valueBodyCapBytes: 16,                  // tiny cap
    });
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);
    // Body well over 16 bytes, valid JSON.
    emitResponseWithBody(proxy, '/heart-rate', JSON.stringify({ bpm: 70, padding: 'xxxxxxxxxxxxxxxx' }));
    expect(diag.snapshot().values).toEqual({});
  });

  it('survives non-JSON bodies and extractor exceptions', () => {
    const diag = createProxyDiag('vitals', {
      valueExtractors: {
        '/heart-rate': (j: any) => ({ bpm: j?.bpm }),
        '/breathing':  () => { throw new Error('boom'); },
      },
    });
    const proxy = new EventEmitter();
    diag.attachToProxy(proxy);
    emitResponseWithBody(proxy, '/heart-rate', '<html>error</html>');         // not JSON
    emitResponseWithBody(proxy, '/breathing',  JSON.stringify({ rpm: 16 }));  // extractor throws
    expect(diag.snapshot().values).toEqual({});
    // Importantly the round-trip stats still landed:
    expect(diag.snapshot().total).toBe(2);
  });

  it('snapshot prunes samples older than the window', () => {
    const diag = createProxyDiag('vitals', {
      valueExtractors: {},
      valueWindowMs: 1000,
    });
    const now = Date.now();
    diag._recordValue('bpm', 70, now - 5000);  // outside window
    diag._recordValue('bpm', 72, now - 200);   // inside window
    const snap = diag.snapshot();
    expect(snap.values?.bpm?.count).toBe(1);
    expect(snap.values?.bpm?.last).toBe(72);
  });

  it('reset() clears value samples', () => {
    const diag = createProxyDiag('vitals', { valueExtractors: { '/x': (j: any) => ({ a: j?.a }) } });
    diag._recordValue('a', 10);
    diag.reset();
    expect(diag.snapshot().values).toEqual({});
  });
});

/* ─── middleware ───────────────────────────────────────────────────── */

describe('diagMiddleware', () => {
  it('returns the JSON snapshot on GET /', () => {
    const diag = createProxyDiag('vitals');
    diag._record('/heart-rate', 5, true, '200');
    const mw = diagMiddleware(diag);
    const req = fakeReq('/');
    const res = fakeRes();
    mw(req, res);
    expect(res._headers['content-type']).toMatch(/application\/json/);
    expect(res._headers['cache-control']).toBe('no-store');
    const parsed = JSON.parse(res._written);
    expect(parsed.name).toBe('vitals');
    expect(parsed.total).toBe(1);
  });

  it('clears state on POST /reset and returns 204', () => {
    const diag = createProxyDiag('vitals');
    diag._record('/heart-rate', 5, true, '200');
    const mw = diagMiddleware(diag);
    const req = fakeReq('/reset', 'POST');
    const res = fakeRes();
    mw(req, res);
    expect(res._status).toBe(204);
    expect(diag.snapshot().total).toBe(0);
  });

  it('forwards thrown errors to next() if provided', () => {
    const diag = createProxyDiag('vitals');
    // Force snapshot() to throw by stubbing it.
    vi.spyOn(diag, 'snapshot').mockImplementation(() => { throw new Error('boom'); });
    const mw = diagMiddleware(diag);
    const next = vi.fn();
    mw(fakeReq('/'), fakeRes(), next);
    expect(next).toHaveBeenCalledWith(expect.objectContaining({ message: 'boom' }));
  });
});
