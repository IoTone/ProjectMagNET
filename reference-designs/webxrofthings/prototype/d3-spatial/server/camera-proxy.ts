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

const PORT = parseInt(process.env.CAMERA_PROXY_PORT ?? '3002', 10);
const CAMERA_HOST = process.env.CAMERA_HOST ?? '10.0.0.185';
const CAMERA_PORT = parseInt(process.env.CAMERA_PORT ?? '80', 10);

const agent = new http.Agent({ family: 4, keepAlive: false });

const server = http.createServer((req, res) => {
  // CORS preflight
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
    if (k === 'host' || k === 'user-agent' || k === 'accept') {
      cleanHeaders[k] = value as string;
    }
    // Drop: cf-*, x-forwarded-*, cookie, referer, origin, accept-language, etc.
  }

  const upstream = http.request(
    {
      hostname: CAMERA_HOST,
      port: CAMERA_PORT,
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
    console.error(`[camera-proxy] upstream error: ${err.message}`);
    if (!res.headersSent) {
      res.writeHead(502, { 'Access-Control-Allow-Origin': '*' });
      res.end(`upstream error: ${err.message}`);
    }
  });

  req.pipe(upstream);
});

server.listen(PORT, () => {
  console.log(`[camera-proxy] Listening on http://localhost:${PORT}`);
  console.log(`[camera-proxy] Forwarding to http://${CAMERA_HOST}:${CAMERA_PORT}`);
  console.log(`[camera-proxy] Try: http://localhost:${PORT}/stream`);
});
