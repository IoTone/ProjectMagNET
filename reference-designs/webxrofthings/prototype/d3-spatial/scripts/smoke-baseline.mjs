#!/usr/bin/env node
/**
 * smoke-baseline.mjs — promote the current `demo/shots/*.png` run into
 * `demo/shots-baseline/*.png`. Run after intentional UI changes; commit
 * the new baseline alongside the code change.
 *
 * Removes baseline files that no longer exist in the current run, so the
 * baseline stays in sync rather than growing forever.
 */

import { readdirSync, copyFileSync, unlinkSync, mkdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

const SHOTS_DIR    = 'demo/shots';
const BASELINE_DIR = 'demo/shots-baseline';

function dirExists(p) { try { return statSync(p).isDirectory(); } catch { return false; } }

if (!dirExists(SHOTS_DIR)) {
  console.error(`no ${SHOTS_DIR}/ — run \`npm run smoke\` first`);
  process.exit(2);
}
mkdirSync(BASELINE_DIR, { recursive: true });

const shots    = new Set(readdirSync(SHOTS_DIR).filter(f => f.endsWith('.png')));
const baseline = new Set(readdirSync(BASELINE_DIR).filter(f => f.endsWith('.png')));

let copied = 0, removed = 0;
for (const f of shots) {
  copyFileSync(join(SHOTS_DIR, f), join(BASELINE_DIR, f));
  copied++;
}
for (const f of baseline) {
  if (!shots.has(f)) {
    unlinkSync(join(BASELINE_DIR, f));
    removed++;
  }
}

console.log(`baseline updated: ${copied} shot(s) copied, ${removed} stale removed`);
console.log(`commit ${BASELINE_DIR}/ to lock the new baseline.`);
