/**
 * Tiny CORS-adding proxy for the ESP32-CAM MJPEG stream.
 *
 * Why: the ESP32-CAM doesn't send CORS headers, so when the XR page is
 * served from a different origin (e.g. a cloudflared tunnel URL), the
 * browser blocks the image load. This proxy forwards the stream and
 * adds `Access-Control-Allow-Origin: *`.
 *
 * Run:
 *   CAMERA_HOST=10.0.0.185 npx tsx server/camera-proxy.ts
 *
 * Then expose it via cloudflared:
 *   cloudflared tunnel --url http://localhost:3002
 *
 * Use the cloudflared URL + /stream as VITE_CAMERA_URL.
 */

import http from 'http';
import { fileURLToPath } from 'url';

export interface CameraProxyOptions {
  cameraHost: string;
  cameraPort?: number;
  /** Headers to forward to the camera (lower-cased). Default: host, user-agent, accept. */
  forwardHeaders?: string[];
  /** Override agent (tests use a custom agent). Default: IPv4-only, keep-alive off. */
  agent?: http.Agent;
}

const DEFAULT_FORWARD = ['host', 'user-agent', 'accept'];

/** Build a CORS-adding HTTP proxy. Returns the http.Server (unstarted). */
export function createCameraProxy(opts: CameraProxyOptions): http.Server {
  const cameraHost = opts.cameraHost;
  const cameraPort = opts.cameraPort ?? 80;
  const forward = new Set((opts.forwardHeaders ?? DEFAULT_FORWARD).map(h => h.toLowerCase()));
  const agent = opts.agent ?? new http.Agent({ family: 4, keepAlive: false });

  return http.createServer((req, res) => {
    if (req.method === 'OPTIONS') {
      res.writeHead(204, {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Methods': 'GET, OPTIONS',
        'Access-Control-Allow-Headers': '*',
        'Access-Control-Max-Age': '86400',
      });
      res.end();
      return;
    }

    // Strip bloated proxy headers before forwarding — ESP32-CAM has a tiny header buffer
    const cleanHeaders: http.OutgoingHttpHeaders = {};
    for (const [key, value] of Object.entries(req.headers)) {
      const k = key.toLowerCase();
      if (forward.has(k)) {
        cleanHeaders[k] = value as string;
      }
    }

    const upstream = http.request(
      {
        hostname: cameraHost,
        port: cameraPort,
        path: req.url,
        method: req.method,
        headers: cleanHeaders,
        agent,
      },
      (upstreamRes) => {
        const headers = { ...upstreamRes.headers };
        headers['access-control-allow-origin'] = '*';
        headers['access-control-allow-methods'] = 'GET, OPTIONS';
        headers['cache-control'] = 'no-cache, no-store, must-revalidate';
        res.writeHead(upstreamRes.statusCode ?? 200, headers);
        upstreamRes.pipe(res);
      },
    );

    upstream.on('error', (err) => {
      if (!res.headersSent) {
        res.writeHead(502, { 'Access-Control-Allow-Origin': '*' });
        res.end(`upstream error: ${err.message}`);
      }
    });

    req.pipe(upstream);
  });
}

// ─── CLI entry point ────────────────────────────────────────────────

const isMain = process.argv[1] === fileURLToPath(import.meta.url);
if (isMain) {
  const PORT = parseInt(process.env.CAMERA_PROXY_PORT ?? '3002', 10);
  const CAMERA_HOST = process.env.CAMERA_HOST ?? '10.0.0.185';
  const CAMERA_PORT = parseInt(process.env.CAMERA_PORT ?? '80', 10);

  const server = createCameraProxy({ cameraHost: CAMERA_HOST, cameraPort: CAMERA_PORT });

  server.on('clientError', (err) => {
    console.error(`[camera-proxy] client error: ${err.message}`);
  });

  server.listen(PORT, () => {
    console.log(`[camera-proxy] Listening on http://localhost:${PORT}`);
    console.log(`[camera-proxy] Forwarding to http://${CAMERA_HOST}:${CAMERA_PORT}`);
    console.log(`[camera-proxy] Try: http://localhost:${PORT}/stream`);
  });
}
