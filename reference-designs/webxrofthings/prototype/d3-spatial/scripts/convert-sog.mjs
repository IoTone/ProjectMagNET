#!/usr/bin/env node
/**
 * convert-sog.mjs — one-time SOG → compressed-PLY conversion for UC4.
 *
 * `@mkkellogg/gaussian-splats-3d` doesn't support `.sog` (the format
 * ml-sharp emits). `@playcanvas/splat-transform` does the conversion
 * cleanly; run this script once after copying the .sog files into
 * `public/spatial/` and the matching `.compressed.ply` files land next
 * to them. The runtime loader points at the .ply file, not the .sog.
 *
 * Usage:
 *   node scripts/convert-sog.mjs        # converts every .sog in public/spatial/
 *   node scripts/convert-sog.mjs file   # converts one file
 *
 * Idempotent: skips any .sog whose .compressed.ply already exists and is
 * newer than the source.
 */
import { readdirSync, statSync, existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const SPATIAL_DIR = join(__dirname, '..', 'public', 'spatial');

const targets = process.argv.slice(2);
const sources = targets.length > 0
  ? targets
  : readdirSync(SPATIAL_DIR)
      .filter(f => f.endsWith('.sog'))
      .map(f => join(SPATIAL_DIR, f));

if (sources.length === 0) {
  console.error(`No .sog files found in ${SPATIAL_DIR}.`);
  console.error('Copy the source files from ml-sharp/samples/ first — see test-plan.md §6.3.1.1.');
  process.exit(1);
}

let converted = 0;
let skipped = 0;
let failed = 0;

for (const src of sources) {
  const out = src.replace(/\.sog$/, '.compressed.ply');
  if (existsSync(out) && statSync(out).mtimeMs >= statSync(src).mtimeMs) {
    console.log(`✓ skip   ${basename(out)}  (already up-to-date)`);
    skipped++;
    continue;
  }

  console.log(`→ convert ${basename(src)} → ${basename(out)} ...`);
  const res = spawnSync('npx', ['splat-transform', src, out], {
    cwd: __dirname,
    stdio: 'inherit',
  });
  if (res.status === 0 && existsSync(out)) {
    converted++;
  } else {
    console.error(`✗ failed  ${basename(src)} (exit ${res.status})`);
    failed++;
  }
}

console.log(`\nDone — converted: ${converted}, skipped: ${skipped}, failed: ${failed}`);
process.exit(failed > 0 ? 1 : 0);
