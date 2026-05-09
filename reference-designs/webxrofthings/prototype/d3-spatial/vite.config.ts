import { defineConfig } from 'vite';
import http from 'http';

const cameraAgent = new http.Agent({ family: 4, keepAlive: false, timeout: 30000 });

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
      // MagNET Vitals device — UC3 personal-health dataspace.
      // Override VITALS_HOST in your shell when the device IP changes:
      //   VITALS_HOST=http://192.168.1.42 npm run dev
      // Must appear BEFORE the generic '/api/v1' entry so the more-specific
      // prefix matches first. Strips the prefix so /api/v1/vitals/heart-rate
      // → /heart-rate on the device. Strips browser headers ESP-IDF's
      // esp_http_server's default 512-byte header buffer can't hold (cookies,
      // sec-ch-*, accept-language, referer) — same pattern as the camera proxy.
      '/api/v1/vitals': {
        target: process.env.VITALS_HOST || 'http://magnet-vitals.local',
        changeOrigin: true,
        rewrite: (path: string) => path.replace(/^\/api\/v1\/vitals/, ''),
        configure: (proxy: any) => {
          proxy.on('proxyReq', (proxyReq: any) => {
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
      },
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
        target: process.env.CAMERA_HOST || 'http://magnet-cam-8610.local',
        changeOrigin: true,
        rewrite: (path: string) => path.replace(/^\/camera/, ''),
        agent: cameraAgent,
        configure: (proxy: any) => {
          proxy.on('proxyReq', (proxyReq: any) => {
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
});
