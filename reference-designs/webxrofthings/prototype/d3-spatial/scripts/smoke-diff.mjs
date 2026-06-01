#!/usr/bin/env node
/**
 * smoke-diff.mjs — visual regression check.
 *
 * Compares every PNG in `demo/shots/` (the most recent smoke run output) against
 * the matching PNG in `demo/shots-baseline/` (the committed reference set) using
 * pixelmatch. Writes per-shot diff PNGs to `demo/shots-diff/` for any shot that
 * exceeds the per-pixel threshold + max-diff-pixels budget. Exits 0 on full pass,
 * 1 on any regression, 2 on usage error.
 *
 * Workflow:
 *   npm run smoke           # produces demo/shots/*.png  (~4 min)
 *   npm run smoke:diff      # this script   (~30 s)
 *   # if a regression is intentional:
 *   npm run smoke:baseline  # promotes the new shots into the baseline
 *
 * Knobs:
 *   --threshold=0.1         pixelmatch per-pixel YIQ tolerance, 0..1 (default 0.1)
 *   --max-diff-ratio=0.005  fraction of pixels that may differ (default 0.5%)
 *   --quiet                 only print on failure
 */

import { readFileSync, writeFileSync, readdirSync, mkdirSync, statSync, existsSync } from 'node:fs';
import { join, basename } from 'node:path';
import pixelmatch from 'pixelmatch';
import { PNG } from 'pngjs';

const SHOTS_DIR    = 'demo/shots';
const BASELINE_DIR = 'demo/shots-baseline';
const DIFF_DIR     = 'demo/shots-diff';

/* ─── argv ──────────────────────────────────────────────────────────── */

let threshold     = 0.1;
let maxDiffRatio  = 0.005;
let quiet         = false;
for (const a of process.argv.slice(2)) {
  if (a.startsWith('--threshold='))      threshold = parseFloat(a.slice('--threshold='.length));
  else if (a.startsWith('--max-diff-ratio=')) maxDiffRatio = parseFloat(a.slice('--max-diff-ratio='.length));
  else if (a === '--quiet')              quiet = true;
  else if (a === '-h' || a === '--help') { usage(); process.exit(0); }
  else                                    { console.error(`unknown arg: ${a}`); usage(); process.exit(2); }
}

function usage() {
  console.error('usage: scripts/smoke-diff.mjs [--threshold=0.1] [--max-diff-ratio=0.005] [--quiet]');
}

/* ─── color (only emit ANSI when stdout is a TTY) ───────────────────── */

const tty   = process.stdout.isTTY;
const RED   = tty ? '\x1b[31m' : '';
const GREEN = tty ? '\x1b[32m' : '';
const YEL   = tty ? '\x1b[33m' : '';
const DIM   = tty ? '\x1b[2m'  : '';
const RESET = tty ? '\x1b[0m'  : '';

/* ─── prereqs ───────────────────────────────────────────────────────── */

function dirExists(p) { try { return statSync(p).isDirectory(); } catch { return false; } }

if (!dirExists(SHOTS_DIR)) {
  console.error(`${RED}no ${SHOTS_DIR}/ — run \`npm run smoke\` first${RESET}`);
  process.exit(2);
}
if (!dirExists(BASELINE_DIR)) {
  console.error(`${RED}no ${BASELINE_DIR}/ — run \`npm run smoke:baseline\` to seed it from the current shots${RESET}`);
  process.exit(2);
}

mkdirSync(DIFF_DIR, { recursive: true });

/* ─── enumerate shots ───────────────────────────────────────────────── */

const shotFiles    = readdirSync(SHOTS_DIR).filter(f => f.endsWith('.png')).sort();
const baselineFiles = new Set(readdirSync(BASELINE_DIR).filter(f => f.endsWith('.png')));

let pass = 0;
let fail = 0;
let missingFromBaseline = 0;
let missingFromCurrent  = 0;
const failures = [];

if (!quiet) {
  console.log(`smoke-diff: ${shotFiles.length} shots in ${SHOTS_DIR}/  vs  ${baselineFiles.size} in ${BASELINE_DIR}/`);
  console.log(`            threshold=${threshold}  max-diff-ratio=${maxDiffRatio}\n`);
}

/* ─── per-shot compare ──────────────────────────────────────────────── */

for (const file of shotFiles) {
  if (!baselineFiles.has(file)) {
    missingFromBaseline++;
    if (!quiet) console.log(`  ${YEL}NEW   ${file}${RESET}  ${DIM}— no baseline yet${RESET}`);
    continue;
  }

  const cur = PNG.sync.read(readFileSync(join(SHOTS_DIR, file)));
  const ref = PNG.sync.read(readFileSync(join(BASELINE_DIR, file)));

  if (cur.width !== ref.width || cur.height !== ref.height) {
    fail++;
    failures.push(file);
    console.log(`  ${RED}SIZE  ${file}${RESET}  current ${cur.width}×${cur.height}  baseline ${ref.width}×${ref.height}`);
    continue;
  }

  const diff = new PNG({ width: cur.width, height: cur.height });
  const diffPixels = pixelmatch(cur.data, ref.data, diff.data, cur.width, cur.height, {
    threshold,
    includeAA: false,
    alpha: 0.4,
    diffColor: [255, 0, 0],
  });
  const ratio = diffPixels / (cur.width * cur.height);

  if (ratio > maxDiffRatio) {
    fail++;
    failures.push(file);
    writeFileSync(join(DIFF_DIR, file), PNG.sync.write(diff));
    console.log(
      `  ${RED}FAIL  ${file}${RESET}  ${diffPixels.toLocaleString()} px differ  ` +
      `(${(ratio * 100).toFixed(3)}% > ${(maxDiffRatio * 100).toFixed(3)}%)  ` +
      `${DIM}→ ${join(DIFF_DIR, file)}${RESET}`,
    );
  } else {
    pass++;
    if (!quiet) {
      const tail = ratio === 0 ? 'identical' : `${diffPixels} px (${(ratio * 100).toFixed(3)}%)`;
      console.log(`  ${GREEN}PASS  ${file}${RESET}  ${DIM}${tail}${RESET}`);
    }
  }
}

/* ─── shots removed since baseline ─────────────────────────────────── */

for (const file of baselineFiles) {
  if (!shotFiles.includes(file)) {
    missingFromCurrent++;
    console.log(`  ${YEL}MISS  ${file}${RESET}  ${DIM}— in baseline but not current run${RESET}`);
  }
}

/* ─── summary ───────────────────────────────────────────────────────── */

console.log('');
const total = shotFiles.length;
const summary =
  `${pass}/${total} pass · ${fail} fail · ${missingFromBaseline} new · ${missingFromCurrent} removed`;
if (fail === 0 && missingFromCurrent === 0) {
  console.log(`${GREEN}${summary}${RESET}`);
  if (missingFromBaseline > 0) {
    console.log(`${DIM}note: ${missingFromBaseline} new shot(s) — promote with \`npm run smoke:baseline\` if intentional${RESET}`);
  }
  process.exit(0);
}
console.log(`${RED}${summary}${RESET}`);
if (failures.length > 0) {
  console.log('failed shots:');
  for (const f of failures) console.log(`  - ${f}`);
}
console.log(`diff PNGs written to ${DIFF_DIR}/`);
process.exit(1);
