import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['src/**/*.{test,spec}.ts', 'server/**/*.{test,spec}.ts'],
    environment: 'node',
    coverage: {
      provider: 'v8',
      reporter: ['text', 'html'],
      include: ['src/util/**', 'src/manifest/**', 'server/**'],
      exclude: ['**/*.{test,spec}.ts', '**/types/**'],
    },
  },
});
