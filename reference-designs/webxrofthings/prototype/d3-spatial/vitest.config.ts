import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['src/**/*.{test,spec}.ts', 'server/**/*.{test,spec}.ts'],
    environment: 'node',
    coverage: {
      provider: 'v8',
      reporter: ['text', 'html'],
      // Include only the modules we actually have unit/integration tests for.
      // Other viz builders (force, pack, sankey, etc.) are exercised visually
      // by the smoke harness; including them here would hide our real coverage
      // posture under a sea of untested file noise.
      include: [
        'src/util/**',
        'src/manifest/**',
        'src/viz/streamgraph.ts',
        'src/demo/liveVitalsCells.ts',
        'server/**',
      ],
      exclude: ['**/*.{test,spec}.ts', '**/types/**'],
    },
  },
});
