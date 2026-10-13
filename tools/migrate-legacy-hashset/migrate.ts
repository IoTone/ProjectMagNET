/**
 * migrate.ts — convert iotonekit-style colon-path HASH-SET records to UDM-MagNET v1.0.
 *
 * See ProjectMagNET/specs/UDM-MagNET-v1.md §6.4 (legacy form) and §6.3 (target form).
 *
 * Run:
 *   npx tsx migrate.ts <input-file>            # prints v1.0 JSON to stdout, warnings to stderr
 *   npx tsx migrate.ts <input-file> --pretty   # pretty-print JSON
 *
 * Limits: this is "best-effort" migration. The legacy form encodes Zigbee-specific
 * concepts (numeric endpoints, capability address/value pairs in the key) that do
 * not map cleanly onto UDM-MagNET v1.0, where services are first-class via USM.
 * Migrated records carry a `udm_endpoints_x` array preserving the legacy structure;
 * downstream tooling should review and re-model as USM services.
 */

import { readFileSync } from 'fs';

export interface KvPair { key: string; value: string; }

export interface MigratedDevice {
  udm_version: '1.0';
  /** Set if input declared a different `udm_version`. Preserved for traceability. */
  udm_source_version_x?: string;
  _comment_migration?: string;
  udm_endpoints_x?: LegacyEndpoint[];
  [field: string]: unknown;
}

/** Preserved Zigbee-flavored endpoint structure from the legacy form. */
export interface LegacyEndpoint {
  endpoint_index: number;
  /** High-level type label (e.g. "OnOff Switch"). */
  udm_type?: string;
  /** Numeric type id from the legacy value (e.g. Zigbee device id "0103"). */
  udm_type_id_x?: string;
  /** Capability list; each entry has the legacy address `id` and current `value`. */
  udm_capabilities_x?: Array<{ name: string; id?: string; value?: string }>;
  [field: string]: unknown;
}

/** UDM-MagNET v1.0 §8 deprecated-alias renames applied at top level. */
const TOP_LEVEL_RENAMES: Record<string, string> = {
  udm_oem: 'udm_vendor',
};

/** Stutter forms inside `udm_chipset_details`. Not currently emitted by the
 *  iotonekit example but covered defensively. */
const CHIPSET_STUTTER: Record<string, string> = {
  udm_chipset_vendor: 'vendor',
  udm_chipset_type: 'type',
  udm_chipset_inst_set: 'instruction_set',
  udm_cpu_max_frequency: 'frequency',
  udm_cpu_number_of_cores: 'core_count',
};

// ─── Parsing ────────────────────────────────────────────────────────

function unquote(s: string): string {
  s = s.trim();
  if (s.length >= 2 && s.startsWith('"') && s.endsWith('"')) return s.slice(1, -1);
  return s;
}

function isCommentLine(line: string): boolean {
  return line.startsWith('#') || line.startsWith('//');
}

/** Parse iotonekit-style `K<n>=<key>` / `V<n>=<value>` paired lines OR a simple
 *  `key=value` per-line file. Comment lines (starting with `#` or `//`) are skipped. */
export function parseKvText(text: string): KvPair[] {
  const lines = text.split(/\r?\n/).map(l => l.trim()).filter(l => l && !isCommentLine(l));
  if (lines.length === 0) return [];

  // Detect iotonekit form: at least one line matches K<n>=...
  const iotonekitForm = lines.some(l => /^K\d+\s*=/.test(l));

  if (iotonekitForm) {
    const keyByN = new Map<string, string>();
    const valueByN = new Map<string, string>();
    for (const line of lines) {
      const km = line.match(/^K(\d+)\s*=\s*(.*)$/);
      if (km) { keyByN.set(km[1]!, unquote(km[2]!)); continue; }
      const vm = line.match(/^V(\d+)\s*=\s*(.*)$/);
      if (vm) { valueByN.set(vm[1]!, unquote(vm[2]!)); continue; }
    }
    const pairs: KvPair[] = [];
    const ns = [...keyByN.keys()].sort((a, b) => parseInt(a, 10) - parseInt(b, 10));
    for (const n of ns) {
      const key = keyByN.get(n)!;
      const value = valueByN.get(n);
      if (value === undefined) continue; // unpaired K with no V — drop
      pairs.push({ key, value });
    }
    return pairs;
  }

  // Simple key=value form
  const pairs: KvPair[] = [];
  for (const line of lines) {
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    pairs.push({ key: line.slice(0, eq).trim(), value: unquote(line.slice(eq + 1)) });
  }
  return pairs;
}

// ─── Migration ──────────────────────────────────────────────────────

export interface MigrationResult {
  device: MigratedDevice;
  warnings: string[];
}

export function migrateLegacyHashSet(pairs: KvPair[]): MigrationResult {
  const warnings: string[] = [];
  const device: MigratedDevice = { udm_version: '1.0' };
  const endpoints = new Map<number, LegacyEndpoint>();

  // Track: did the input have its own udm_endpoints_x scalar (count)?
  let scalarEndpointsCount: string | undefined;

  for (const { key, value } of pairs) {
    // Strip ":_" suffix marker (means "the value/invocation of the underscore form").
    let isInvocation = false;
    let working = key;
    if (working.endsWith(':_')) { isInvocation = true; working = working.slice(0, -2); }

    // Endpoint-prefixed: "<n>:<rest>"
    const ep = working.match(/^(\d+):(.+)$/);
    if (ep) {
      const epIdx = parseInt(ep[1]!, 10);
      const rest = ep[2]!;
      const slot = endpoints.get(epIdx) ?? { endpoint_index: epIdx };
      endpoints.set(epIdx, slot);

      const innerColon = rest.indexOf(':');
      if (innerColon > 0) {
        const attr = rest.slice(0, innerColon);
        const name = rest.slice(innerColon + 1);
        if (attr === 'udm_capability_x') {
          const caps = (slot.udm_capabilities_x ??= []);
          let cap = caps.find(c => c.name === name);
          if (!cap) { cap = { name }; caps.push(cap); }
          if (isInvocation) cap.value = value;
          else cap.id = value;
        } else if (attr === 'udm_type') {
          slot.udm_type = name;
          slot.udm_type_id_x = value;
        } else {
          // Generic legacy "<attr>:<name>" pattern preserved verbatim.
          slot[`${attr}_${sanitize(name)}_x`] = value;
          warnings.push(`endpoint ${epIdx}: preserved legacy "${attr}:${name}" verbatim as "${attr}_${sanitize(name)}_x" — review for v1.0 modeling`);
        }
      } else {
        // Bare endpoint attr (e.g. "1:udm_capabilities_x" with comma list)
        if (rest === 'udm_capabilities_x') {
          // Sometimes the names list arrives BEFORE the per-capability rows; only
          // record names if we haven't already populated entries.
          const list = value.split(',').map(s => s.trim()).filter(Boolean);
          if (!slot.udm_capabilities_x) slot.udm_capabilities_x = list.map(name => ({ name }));
        } else {
          slot[rest] = value;
        }
      }
      continue;
    }

    // Top-level. Apply ":_" invocation suffix as a `_value_x` companion field.
    if (isInvocation) {
      const baseRenamed = TOP_LEVEL_RENAMES[working] ?? working;
      device[`${baseRenamed}_value_x`] = value;
      warnings.push(`top-level "${key}" treated as companion value for "${baseRenamed}" → "${baseRenamed}_value_x"`);
      continue;
    }

    if (working === 'udm_endpoints_x') {
      // Scalar count; we may overwrite with the array later.
      scalarEndpointsCount = value;
      continue;
    }

    if (working === 'udm_version') {
      if (value !== '1.0') {
        device.udm_source_version_x = value;
        warnings.push(`udm_version "${value}" in input; rewritten to "1.0" (source preserved as udm_source_version_x)`);
      }
      continue; // device.udm_version already pinned to "1.0"
    }

    // Stutter form inside chipset_details (rare in this format but defensive)
    const csMatch = working.match(/^udm_chipset_details:([^:]+)$/);
    if (csMatch) {
      const sub = csMatch[1]!;
      const renamed = CHIPSET_STUTTER[sub] ?? sub;
      const cs = (device.udm_chipset_details as Record<string, unknown> | undefined) ?? {};
      cs[renamed] = value;
      device.udm_chipset_details = cs;
      if (renamed !== sub) warnings.push(`udm_chipset_details: renamed "${sub}" → "${renamed}" per UDM-MagNET v1.0 §3.3`);
      continue;
    }

    const renamed = TOP_LEVEL_RENAMES[working] ?? working;
    if (renamed !== working) warnings.push(`top-level: renamed "${working}" → "${renamed}" per UDM-MagNET v1.0 §8`);
    device[renamed] = value;
  }

  if (endpoints.size > 0) {
    const sorted = [...endpoints.entries()].sort((a, b) => a[0] - b[0]).map(([, v]) => v);
    device.udm_endpoints_x = sorted;
    if (scalarEndpointsCount !== undefined && parseInt(scalarEndpointsCount, 10) !== sorted.length) {
      warnings.push(`udm_endpoints_x scalar count "${scalarEndpointsCount}" does not match parsed endpoint count ${sorted.length}`);
    }
    warnings.push('udm_endpoints_x is a legacy Zigbee-flavored construct preserved as an extension; v1.0 prefers usm_services');
  } else if (scalarEndpointsCount !== undefined) {
    // Preserve the scalar (no endpoints to expand) as a legacy companion
    device.udm_endpoints_count_x = scalarEndpointsCount;
  }

  device._comment_migration = 'Migrated from iotonekit legacy HASH-SET form by ProjectMagNET/tools/migrate-legacy-hashset. Review udm_endpoints_x and any *_x extensions before promoting to a v1.0 dataspace manifest.';

  return { device, warnings };
}

/** Replace whitespace and unsafe chars in capability/type names so we get clean keys. */
function sanitize(s: string): string {
  return s.toLowerCase().replace(/[^\w]+/g, '-').replace(/^-+|-+$/g, '');
}

// ─── CLI entry ─────────────────────────────────────────────────────

import { fileURLToPath } from 'url';
const isMain = process.argv[1] === fileURLToPath(import.meta.url);
if (isMain) {
  const args = process.argv.slice(2);
  const file = args.find(a => !a.startsWith('--'));
  const pretty = args.includes('--pretty');
  if (!file) {
    console.error('usage: tsx migrate.ts <input-file> [--pretty]');
    process.exit(2);
  }
  const text = readFileSync(file, 'utf-8');
  const pairs = parseKvText(text);
  const { device, warnings } = migrateLegacyHashSet(pairs);
  for (const w of warnings) console.error(`[warn] ${w}`);
  process.stdout.write(JSON.stringify(device, null, pretty ? 2 : 0));
  process.stdout.write('\n');
}
