import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import http from 'http';
import { AddressInfo } from 'net';
import { createCameraProxy } from './camera-proxy';

interface CapturedReq {
  method: string | undefined;
  url: string | undefined;
  headers: http.IncomingHttpHeaders;
}

/** Spin up a fake "ESP32-CAM" that records the request and replies with a JPEG-ish body. */
function startFakeCamera(opts: { delay?: number; status?: number; body?: string } = {}): Promise<{
  port: number;
  captured: CapturedReq[];
  close: () => Promise<void>;
}> {
  const captured: CapturedReq[] = [];
  const server = http.createServer((req, res) => {
    captured.push({ method: req.method, url: req.url, headers: { ...req.headers } });
    setTimeout(() => {
      res.writeHead(opts.status ?? 200, { 'Content-Type': 'image/jpeg' });
      res.end(opts.body ?? 'fake-jpeg');
    }, opts.delay ?? 0);
  });

  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => {
      const port = (server.address() as AddressInfo).port;
      resolve({
        port,
        captured,
        close: () => new Promise<void>((r) => server.close(() => r())),
      });
    });
  });
}

function startProxy(cameraPort: number, opts: Parameters<typeof createCameraProxy>[0] extends infer O ? Partial<Omit<O, 'cameraHost' | 'cameraPort'>> : never = {}): Promise<{
  port: number;
  close: () => Promise<void>;
}> {
  const proxy = createCameraProxy({
    cameraHost: '127.0.0.1',
    cameraPort,
    agent: new http.Agent({ keepAlive: false }),
    ...opts,
  });
  return new Promise((resolve) => {
    proxy.listen(0, '127.0.0.1', () => {
      const port = (proxy.address() as AddressInfo).port;
      resolve({
        port,
        close: () => new Promise<void>((r) => proxy.close(() => r())),
      });
    });
  });
}

function get(port: number, path: string, headers: http.OutgoingHttpHeaders = {}, method = 'GET'): Promise<{
  status: number;
  headers: http.IncomingHttpHeaders;
  body: string;
}> {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, path, method, headers }, (res) => {
      const chunks: Buffer[] = [];
      res.on('data', (c: Buffer) => chunks.push(c));
      res.on('end', () => resolve({
        status: res.statusCode ?? 0,
        headers: res.headers,
        body: Buffer.concat(chunks).toString('utf-8'),
      }));
    });
    req.on('error', reject);
    req.end();
  });
}

describe('camera-proxy — CORS preflight', () => {
  it('responds to OPTIONS with 204 + CORS headers, no upstream call', async () => {
    const cam = await startFakeCamera();
    const proxy = await startProxy(cam.port);

    const res = await get(proxy.port, '/stream', {}, 'OPTIONS');
    expect(res.status).toBe(204);
    expect(res.headers['access-control-allow-origin']).toBe('*');
    expect(res.headers['access-control-allow-methods']).toContain('GET');
    expect(cam.captured).toHaveLength(0);

    await proxy.close();
    await cam.close();
  });
});

describe('camera-proxy — header stripping', () => {
  it('drops cf-*, x-forwarded-*, cookie, referer, origin', async () => {
    const cam = await startFakeCamera();
    const proxy = await startProxy(cam.port);

    await get(proxy.port, '/capture', {
      'cf-connecting-ip': '1.2.3.4',
      'cf-ray': 'abc123',
      'x-forwarded-for': '1.2.3.4',
      'x-forwarded-proto': 'https',
      'cookie': 'session=abc',
      'referer': 'https://attacker.example',
      'origin': 'https://attacker.example',
      'accept-language': 'en-US',
    });

    expect(cam.captured).toHaveLength(1);
    const fwd = cam.captured[0]!.headers;
    expect(fwd['cf-connecting-ip']).toBeUndefined();
    expect(fwd['cf-ray']).toBeUndefined();
    expect(fwd['x-forwarded-for']).toBeUndefined();
    expect(fwd['x-forwarded-proto']).toBeUndefined();
    expect(fwd['cookie']).toBeUndefined();
    expect(fwd['referer']).toBeUndefined();
    expect(fwd['origin']).toBeUndefined();
    expect(fwd['accept-language']).toBeUndefined();

    await proxy.close();
    await cam.close();
  });

  it('forwards host, user-agent, accept by default', async () => {
    const cam = await startFakeCamera();
    const proxy = await startProxy(cam.port);

    await get(proxy.port, '/capture', {
      'user-agent': 'TestAgent/1.0',
      'accept': 'image/jpeg',
    });

    const fwd = cam.captured[0]!.headers;
    expect(fwd['user-agent']).toBe('TestAgent/1.0');
    expect(fwd['accept']).toBe('image/jpeg');

    await proxy.close();
    await cam.close();
  });
});

describe('camera-proxy — response shape', () => {
  it('injects CORS + cache-control on the response', async () => {
    const cam = await startFakeCamera({ status: 200, body: 'ok' });
    const proxy = await startProxy(cam.port);

    const res = await get(proxy.port, '/capture');
    expect(res.status).toBe(200);
    expect(res.headers['access-control-allow-origin']).toBe('*');
    expect(res.headers['cache-control']).toMatch(/no-cache/);
    expect(res.body).toBe('ok');

    await proxy.close();
    await cam.close();
  });

  it('preserves upstream non-200 status codes', async () => {
    const cam = await startFakeCamera({ status: 404 });
    const proxy = await startProxy(cam.port);

    const res = await get(proxy.port, '/missing');
    expect(res.status).toBe(404);
    expect(res.headers['access-control-allow-origin']).toBe('*');

    await proxy.close();
    await cam.close();
  });
});

describe('camera-proxy — upstream failure', () => {
  it('returns 502 when upstream is unreachable', async () => {
    // Allocate a free port, then immediately close — connections to it will refuse
    const sink = http.createServer();
    await new Promise<void>(r => sink.listen(0, '127.0.0.1', () => r()));
    const deadPort = (sink.address() as AddressInfo).port;
    await new Promise<void>(r => sink.close(() => r()));

    const proxy = await startProxy(deadPort);
    const res = await get(proxy.port, '/capture');
    expect(res.status).toBe(502);
    expect(res.headers['access-control-allow-origin']).toBe('*');

    await proxy.close();
  });
});
