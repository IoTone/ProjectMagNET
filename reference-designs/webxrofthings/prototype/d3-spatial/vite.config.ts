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
    proxy: {
      '/api/v1': {
        target: 'http://localhost:3001',
        changeOrigin: true,
      },
      '/camera': {
        target: 'http://10.0.0.185',
        changeOrigin: true,
        rewrite: (path: string) => path.replace(/^\/camera/, ''),
      },
    },
  },
  build: {
    target: 'es2022',
    sourcemap: true,
  },
});
