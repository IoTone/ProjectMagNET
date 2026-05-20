import { defineConfig } from 'vite';
import http from 'http';
import { createProxyDiag, diagMiddleware } from './server/proxy-diag';

// One collector per device. Both attach to their respective proxy entries
// in `configure` below, and both get a Connect middleware mount in the plugin.
//
// Vitals also gets per-URL value extractors so the diag snapshot reports
// rolling stats (mean/min/max/p50/p95) on the actual sensor scalars over
// a 5-minute window — useful for "is the device producing reasonable
// readings?" without correlating browser logs with serial output.
const vitalsDiag = createProxyDiag('vitals', {
  valueExtractors: {
    // Only count BPM when presence=true; otherwise the device returns 0
    // and the mean would skew toward zero whenever the chair is empty.
    '/api/v1/vitals/heart-rate': (j: any) =>
      ({ bpm: j?.presence && typeof j?.bpm === 'number' && j.bpm > 0 ? j.bpm : undefined }),
    '/api/v1/vitals/breathing': (j: any) =>
      ({ rpm: typeof j?.rpm === 'number' && j.rpm > 0 ? j.rpm : undefined }),
    '/api/v1/vitals/targets': (j: any) => {
      const out: Record<string, number | undefined> = {
        target_count: typeof j?.count === 'number' ? j.count : undefined,
      };
      // Distance to the nearest target — telemetry-friendly proxy for "is
      // someone close?". Skip when no targets are present.
      const ts = Array.isArray(j?.targets) ? j.targets : [];
      let nearest = Infinity;
      for (const t of ts) {
        if (typeof t?.x_m === 'number' && typeof t?.y_m === 'number') {
          const d = Math.hypot(t.x_m, t.y_m);
          if (d < nearest) nearest = d;
        }
      }
      if (Number.isFinite(nearest)) out.nearest_m = nearest;
      return out;
    },
    '/api/v1/vitals/presence': (j: any) =>
      ({ presence: typeof j?.presence === 'boolean' ? (j.presence ? 1 : 0) : undefined }),
    '/api/v1/vitals/lux': (j: any) =>
      ({ lux: typeof j?.lux === 'number' ? j.lux : undefined }),
  },
  // 5-minute rolling window — long enough to smooth out per-second jitter,
  // short enough that snapshots reflect "right now" rather than "since boot".
  valueWindowMs: 5 * 60_000,
});
const cameraDiag = createProxyDiag('camera');
const lightingDiag = createProxyDiag('lighting');
const boomboxDiag = createProxyDiag('boombox');
const envDiag = createProxyDiag('env');

/**
 * Defensive normalizer for *_HOST env vars. http-proxy's URL parser yields
 * `hostname: null` for a bare IP / hostname (no scheme), and the downstream
 * `setupOutgoing` then dies with `Cannot read properties of null (reading
 * 'split')`. Auto-prefix `http://` so `LIGHTING_HOST=10.0.0.144 npm run dev`
 * Just Works without crashing the dev server.
 */
const normalizeHost = (h: string | undefined, fallback: string): string => {
  const s = (h && h.trim()) || fallback;
  return /^https?:\/\//.test(s) ? s : `http://${s}`;
};

const cameraAgent = new http.Agent({ family: 4, keepAlive: false, timeout: 30000 });

/**
 * Lighting device agent — XIAO ESP32-C3 running esp_http_server.
 * Same shape as the vitals agent: single socket, no keep-alive, IPv4-only.
 * The C3 has even less RAM than the C6 and tolerates concurrent fetches
 * even worse. Funnel the UC2 actuator panel through one connection.
 */
const lightingAgent = new http.Agent({
  family: 4,
  keepAlive: false,
  maxSockets: 1,
  timeout: 15000,
});

/**
 * Boombox device agent — XIAO ESP32-S3 running esp_http_server.
 * Same single-socket, no-keep-alive, IPv4 pattern as the other ESP-IDF
 * device agents — the speaker endpoint is invoked one chime at a time
 * from the UC2 panel and never needs concurrency.
 */
const boomboxAgent = new http.Agent({
  family: 4,
  keepAlive: false,
  maxSockets: 1,
  timeout: 15000,
});

/**
 * Environment-sensor (AHT20) device agent — M5 Atom Echo + Hex.
 * Same single-socket pattern. Polled cadence from the dataspace is low
 * (sensor task on the device samples at 0.5 Hz; UI panel reads at 1–5 s).
 */
const envAgent = new http.Agent({
  family: 4,
  keepAlive: false,
  maxSockets: 1,
  timeout: 15000,
});

/**
 * Vitals device agent — XIAO ESP32-C6 running esp_http_server.
 *
 * `maxSockets: 1` is the load-bearing setting. Without it, four cells
 * (HR, BR, phases, targets) can have outbound fetches in flight to the
 * device simultaneously, which overwhelms the C6's tiny TCP socket pool
 * and surfaces as `httpd_sock_err: error in send : 11` (EAGAIN — kernel
 * send buffer full) at roughly the cadence of the most-frequent cell.
 * Worst case the WiFi task starves IDLE on CPU 0 and the watchdog fires.
 *
 * Funneling everything through a single proxy socket guarantees the
 * device only ever sees one concurrent request, regardless of how many
 * cells are polling. `keepAlive: false` matches the camera lesson — small
 * ESP-IDF TCP stacks tend to wedge on the second request of a kept-alive
 * session. `family: 4` avoids the IPv6 fallback when DNS resolves both.
 */
const vitalsAgent = new http.Agent({
  family: 4,
  keepAlive: false,
  maxSockets: 1,
  timeout: 15000,
});

export default defineConfig({
  server: {
    host: true,
    https: false,
    allowedHosts: [
      '.trycloudflare.com',
      '.ngrok-free.app',
      '.ngrok.app',
      '.ngrok.io',
      '.loca.lt',
    ],
    proxy: {
      // XIAO_ESP32C3_IOT_LIGHTING — UC2 neopixel actuator.
      // The device's mDNS hostname is MAC-suffixed to match its BLE name
      // (e.g. magnet-lighting-b7c0.local). The default below probably will
      // NOT resolve on a multi-device LAN — set LIGHTING_HOST explicitly:
      //   LIGHTING_HOST=http://magnet-lighting-b7c0.local  npm run dev
      //   LIGHTING_HOST=http://192.168.1.42                npm run dev
      // Must appear BEFORE the generic '/api/v1' entry so this more-specific
      // prefix matches first. The device exposes the URL verbatim
      // (/api/v1/actuator/neopixel) so no rewrite. The other actuator paths
      // (light, thermostat, speaker) fall through to the mock-join-server.
      '/api/v1/actuator/neopixel': {
        target: normalizeHost(process.env.LIGHTING_HOST, 'http://magnet-lighting.local'),
        changeOrigin: true,
        agent: lightingAgent,
        configure: (proxy: any) => {
          lightingDiag.attachToProxy(proxy);
          proxy.on('proxyReq', (proxyReq: any) => {
            // Same race guard + header strip as the vitals/camera proxies —
            // ESP-IDF httpd's small header buffer can't hold full browser
            // headers; CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024 helps but stripping
            // the noisy headers is belt-and-braces.
            if (proxyReq.headersSent) return;
            const drop = [
              'cookie', 'accept-language', 'referer', 'origin',
              'cache-control', 'pragma',
              'sec-ch-ua', 'sec-ch-ua-mobile', 'sec-ch-ua-platform',
              'sec-fetch-dest', 'sec-fetch-mode', 'sec-fetch-site', 'sec-fetch-user',
              'upgrade-insecure-requests',
            ];
            for (const h of drop) proxyReq.removeHeader(h);
          });
        },
      } as any,
      // M5Atom_Echo_Hex_Hive_ATH20 — UC2 environment sensor (temp + humidity).
      // The AHT20 firmware exposes /api/v1/sensor/{environment,temperature,
      // humidity}. Set ENV_HOST to the device IP (mDNS is published via the
      // hive flow, post-SNTP; IP is the reliable default).
      //   ENV_HOST=http://10.0.0.55  npm run dev
      // Must appear BEFORE the generic '/api/v1' entry.
      '/api/v1/sensor': {
        target: normalizeHost(process.env.ENV_HOST, 'http://magnet-atom-echo.local'),
        changeOrigin: true,
        agent: envAgent,
        configure: (proxy: any) => {
          envDiag.attachToProxy(proxy);
          proxy.on('proxyReq', (proxyReq: any) => {
            if (proxyReq.headersSent) return;
            const drop = [
              'cookie', 'accept-language', 'referer', 'origin',
              'cache-control', 'pragma',
              'sec-ch-ua', 'sec-ch-ua-mobile', 'sec-ch-ua-platform',
              'sec-fetch-dest', 'sec-fetch-mode', 'sec-fetch-site', 'sec-fetch-user',
              'upgrade-insecure-requests',
            ];
            for (const h of drop) proxyReq.removeHeader(h);
          });
        },
      } as any,
      // MagNET_ReSpeaker_Boombox — UC2 speaker actuator (chime/doorbell).
      // The boombox firmware exposes /api/v1/actuator/speaker/play directly,
      // matching the in-XR panel's existing POST. mDNS hostname is
      // magnet-boombox-<MAC4>.local but it's published from the hive flow
      // (post-SNTP); set BOOMBOX_HOST to the IP for reliable bring-up.
      //   BOOMBOX_HOST=http://10.0.0.55  npm run dev
      // Must appear BEFORE the generic '/api/v1' entry.
      '/api/v1/actuator/speaker': {
        target: normalizeHost(process.env.BOOMBOX_HOST, 'http://magnet-boombox.local'),
        changeOrigin: true,
        agent: boomboxAgent,
        configure: (proxy: any) => {
          boomboxDiag.attachToProxy(proxy);
          proxy.on('proxyReq', (proxyReq: any) => {
            if (proxyReq.headersSent) return;
            const drop = [
              'cookie', 'accept-language', 'referer', 'origin',
              'cache-control', 'pragma',
              'sec-ch-ua', 'sec-ch-ua-mobile', 'sec-ch-ua-platform',
              'sec-fetch-dest', 'sec-fetch-mode', 'sec-fetch-site', 'sec-fetch-user',
              'upgrade-insecure-requests',
            ];
            for (const h of drop) proxyReq.removeHeader(h);
          });
        },
      } as any,
      // MagNET Vitals device — UC3 personal-health dataspace.
      // Override VITALS_HOST in your shell when the device IP changes:
      //   VITALS_HOST=http://192.168.1.42 npm run dev
      // Must appear BEFORE the generic '/api/v1' entry so the more-specific
      // prefix matches first. Strips the prefix so /api/v1/vitals/heart-rate
      // → /heart-rate on the device. Strips browser headers ESP-IDF's
      // esp_http_server's default 512-byte header buffer can't hold (cookies,
      // sec-ch-*, accept-language, referer) — same pattern as the camera proxy.
      '/api/v1/vitals': {
        target: normalizeHost(process.env.VITALS_HOST, 'http://magnet-vitals.local'),
        changeOrigin: true,
        rewrite: (path: string) => path.replace(/^\/api\/v1\/vitals/, ''),
        agent: vitalsAgent,
        configure: (proxy: any) => {
          vitalsDiag.attachToProxy(proxy);
          proxy.on('proxyReq', (proxyReq: any) => {
            // Race guard: with maxSockets:1 + keepAlive:false, the proxyReq
            // event can fire on a ClientRequest whose headers already flushed
            // (especially when http-proxy retries after the device errors).
            // Calling removeHeader after headersSent throws ERR_HTTP_HEADERS_SENT
            // and crashes Vite. Skip the strip in that window — the request
            // is already on the wire so dropping headers wouldn't help anyway.
            if (proxyReq.headersSent) return;
            const drop = [
              'cookie', 'accept-language', 'referer', 'origin',
              'cache-control', 'pragma',
              'sec-ch-ua', 'sec-ch-ua-mobile', 'sec-ch-ua-platform',
              'sec-fetch-dest', 'sec-fetch-mode', 'sec-fetch-site', 'sec-fetch-user',
              'upgrade-insecure-requests',
            ];
            for (const h of drop) proxyReq.removeHeader(h);
          });
        },
      } as any,
      '/api/v1': {
        target: 'http://localhost:3001',
        changeOrigin: true,
      },
      // ESP32-CAM proxy. Override the host in your shell when DHCP shifts:
      //   CAMERA_HOST=http://192.168.1.55  npm run dev
      //   CAMERA_HOST=http://magnet-cam-8610.local  npm run dev
      // - rewrite drops the `/camera` prefix so /camera/capture → /capture on the device
      // - cameraAgent forces IPv4 + no keep-alive (the ESP32-CAM's tiny TCP stack
      //   gives ECONNRESET on the second request of a keep-alive session and
      //   sometimes resolves over IPv6 with no listener)
      // - configure/proxyReq drops browser headers the camera's 512-byte header
      //   buffer can't hold; same pattern as /api/v1/vitals above
      '/camera': {
        target: normalizeHost(process.env.CAMERA_HOST, 'http://magnet-cam-8610.local'),
        changeOrigin: true,
        rewrite: (path: string) => path.replace(/^\/camera/, ''),
        agent: cameraAgent,
        configure: (proxy: any) => {
          cameraDiag.attachToProxy(proxy);
          proxy.on('proxyReq', (proxyReq: any) => {
            // Same race guard as the vitals proxy — see the long-form note above.
            if (proxyReq.headersSent) return;
            const drop = [
              'cookie', 'accept-language', 'referer', 'origin',
              'cache-control', 'pragma',
              'sec-ch-ua', 'sec-ch-ua-mobile', 'sec-ch-ua-platform',
              'sec-fetch-dest', 'sec-fetch-mode', 'sec-fetch-site', 'sec-fetch-user',
              'upgrade-insecure-requests',
            ];
            for (const h of drop) proxyReq.removeHeader(h);
          });
        },
      } as any,
    },
  },
  build: {
    target: 'es2022',
    sourcemap: true,
  },
  plugins: [
    {
      // Diagnostic endpoints — observe what the proxy sees, no firmware needed.
      // GET  /__diag/vitals          → JSON snapshot of recent traffic to /api/v1/vitals/*
      // POST /__diag/vitals/reset    → clear counters
      // (Same for /__diag/camera against the /camera proxy.)
      // Mount path is stripped by Connect, so the middleware sees req.url='/' or '/reset'.
      name: 'proxy-diag',
      configureServer(server) {
        server.middlewares.use('/__diag/vitals', diagMiddleware(vitalsDiag));
        server.middlewares.use('/__diag/camera', diagMiddleware(cameraDiag));
        server.middlewares.use('/__diag/lighting', diagMiddleware(lightingDiag));
        server.middlewares.use('/__diag/boombox', diagMiddleware(boomboxDiag));
        server.middlewares.use('/__diag/env', diagMiddleware(envDiag));
      },
    },
  ],
});
