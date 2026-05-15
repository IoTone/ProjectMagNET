#!/usr/bin/env node
/**
 * convert-sog.mjs — SOG/PLY → compressed-PLY conversion for UC4.
 *
 * `@mkkellogg/gaussian-splats-3d` doesn't support `.sog` (the format
 * ml-sharp emits). `@playcanvas/splat-transform` does the conversion
 * cleanly; this script wraps it with the project's chosen perf settings
 * (decimation in particular — see below).
 *
 * Inputs accepted: `.sog` and `.ply` files in `public/spatial/`.
 * Output:         `<basename>.compressed.ply` next to each source.
 *
 * Usage:
 *   npm run convert:spatial                       # convert everything
 *   node scripts/convert-sog.mjs <file>           # convert one file
 *   SPLAT_DECIMATE=25% npm run convert:spatial    # different decimation
 *   SPLAT_DECIMATE=0 npm run convert:spatial      # full-quality (no decimation)
 *
 * ── Why decimate? ─────────────────────────────────────────────────────
 *
 * The raw photogrammetry captures arrive with ~1.1M Gaussian splats each
 * (file size ~19 MB after default splat-transform compression). At runtime
 * mkkellogg renders that as ~2.3M triangles per frame — exceeding Quest 3's
 * fragment budget at 90 Hz and crashing Spectacles. Empirically we saw
 * 8-18 FPS in browser, hand tracking dropping out on Quest, viewport
 * lockup on Spectacles.
 *
 * `--decimate 10%` (the default below) uses splat-transform's pairwise-
 * merging algorithm to reduce that to ~118 K splats per scene. Unlike
 * alpha thresholding, this preserves visual coverage — neighbours merge
 * weighted by perceptual contribution rather than dropping the faintest
 * splats and leaving holes. File size: 19 MB → 1.8 MB, ~10× cut in both
 * load time and runtime cost.
 *
 * Override with `SPLAT_DECIMATE` env var. Set to `0` (or `none`) for full
 * quality (use when targeting desktop-only deployments).
 *
 * Idempotent: skips any source whose .compressed.ply already exists and
 * is newer than both the source AND the script itself, so a change to the
 * decimation setting forces re-conversion.
 */
import { readdirSync, statSync, existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const SPATIAL_DIR = join(__dirname, '..', 'public', 'spatial');
const SCRIPT_PATH = fileURLToPath(import.meta.url);

/** Decimation level. Accepts `<n>` (absolute splat count) or `<n>%` (fraction
 *  of input). Set to '0' / 'none' / '' to skip decimation entirely. */
const decimate = process.env.SPLAT_DECIMATE ?? '10%';
const wantDecimate = decimate && decimate !== '0' && decimate.toLowerCase() !== 'none';

const targets = process.argv.slice(2);
const sources = targets.length > 0
  ? targets
  : readdirSync(SPATIAL_DIR)
      .filter(f => f.endsWith('.sog') || f.endsWith('.ply'))
      .filter(f => !f.endsWith('.compressed.ply'))   // skip already-converted outputs
      .map(f => join(SPATIAL_DIR, f));

if (sources.length === 0) {
  console.error(`No .sog or .ply files found in ${SPATIAL_DIR}.`);
  console.error('Copy the source files from ml-sharp/samples/ first — see test-plan.md §6.3.1.1.');
  process.exit(1);
}

console.log(`Decimation: ${wantDecimate ? decimate : 'none (full quality)'}`);

let converted = 0;
let skipped = 0;
let failed = 0;

for (const src of sources) {
  const out = src.replace(/\.(sog|ply)$/, '.compressed.ply');
  // The script's own mtime is part of the freshness check so any future
  // edit to decimation settings forces re-conversion of every asset.
  const scriptMtime = statSync(SCRIPT_PATH).mtimeMs;
  if (existsSync(out)
        && statSync(out).mtimeMs >= statSync(src).mtimeMs
        && statSync(out).mtimeMs >= scriptMtime) {
    console.log(`✓ skip   ${basename(out)}  (already up-to-date)`);
    skipped++;
    continue;
  }

  console.log(`→ convert ${basename(src)} → ${basename(out)} ...`);
  const args = ['splat-transform', src];
  if (wantDecimate) args.push('--decimate', decimate);
  args.push('--overwrite', out);
  const res = spawnSync('npx', args, {
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
