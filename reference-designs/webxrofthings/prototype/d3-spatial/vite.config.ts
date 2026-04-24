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
      '/api/v1': {
        target: 'http://localhost:3001',
        changeOrigin: true,
      },
      '/camera': {
        target: 'http://10.0.0.185',
        changeOrigin: true,
        rewrite: (path: string) => path.replace(/^\/camera/, ''),
        agent: cameraAgent,
      } as any,
    },
  },
  build: {
    target: 'es2022',
    sourcemap: true,
  },
});
