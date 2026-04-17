import { defineConfig } from 'vite';

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
  },
  build: {
    target: 'es2022',
    sourcemap: true,
  },
});
