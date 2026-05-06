import { describe, it, expect } from 'vitest';
import { readFileSync } from 'fs';
import { join } from 'path';
import { fileURLToPath } from 'url';
import { dirname } from 'path';
import { validateManifest, EXAMPLE_MANIFEST } from './schema';

const __dirname = dirname(fileURLToPath(import.meta.url));
const EXAMPLES_DIR = join(__dirname, '..', '..', 'examples');

function loadFixture(name: string): unknown {
  return JSON.parse(readFileSync(join(EXAMPLES_DIR, name), 'utf-8'));
}

describe('validateManifest — fixtures', () => {
  it('accepts examples/uc2-room.json', () => {
    const result = validateManifest(loadFixture('uc2-room.json'));
    if (!result.valid) console.error(result.errors);
    expect(result.valid).toBe(true);
  });

  it('accepts examples/room-dataspace.json', () => {
    const result = validateManifest(loadFixture('room-dataspace.json'));
    if (!result.valid) console.error(result.errors);
    expect(result.valid).toBe(true);
  });

  it('accepts the in-source EXAMPLE_MANIFEST constant', () => {
    expect(validateManifest(EXAMPLE_MANIFEST).valid).toBe(true);
  });
});

describe('validateManifest — top-level errors', () => {
  it('rejects null', () => {
    const r = validateManifest(null);
    expect(r.valid).toBe(false);
  });

  it('rejects a non-object', () => {
    const r = validateManifest('not a manifest');
    expect(r.valid).toBe(false);
  });

  it('rejects missing version', () => {
    const r = validateManifest({ name: 'x', scaleTag: 'room', marks: [] });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/version/);
  });

  it('rejects wrong version', () => {
    const r = validateManifest({ version: '2', name: 'x', scaleTag: 'room', marks: [] });
    expect(r.valid).toBe(false);
  });

  it('rejects empty name', () => {
    const r = validateManifest({ version: '1', name: '', scaleTag: 'room', marks: [] });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/name/);
  });

  it('rejects unknown scaleTag', () => {
    const r = validateManifest({ version: '1', name: 'x', scaleTag: 'galaxy', marks: [] });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/scaleTag/);
  });

  it('rejects non-array marks', () => {
    const r = validateManifest({ version: '1', name: 'x', scaleTag: 'room', marks: 'oops' });
    expect(r.valid).toBe(false);
  });

  it('accepts empty marks array', () => {
    const r = validateManifest({ version: '1', name: 'x', scaleTag: 'room', marks: [] });
    expect(r.valid).toBe(true);
  });
});

describe('validateManifest — mark errors', () => {
  const base = { version: '1' as const, name: 'x', scaleTag: 'room' as const };

  it('rejects unknown mark type', () => {
    const r = validateManifest({
      ...base,
      marks: [{ id: 'm1', type: 'donut-of-doom', title: 't', data: { source: 'inline' } }],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/donut-of-doom/);
  });

  it('rejects missing data', () => {
    const r = validateManifest({
      ...base,
      marks: [{ id: 'm1', type: 'tree', title: 't' }],
    });
    expect(r.valid).toBe(false);
  });

  it('rejects unknown data.source', () => {
    const r = validateManifest({
      ...base,
      marks: [{ id: 'm1', type: 'tree', title: 't', data: { source: 'magic' } }],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/source/);
  });

  it('rejects url data without url', () => {
    const r = validateManifest({
      ...base,
      marks: [{ id: 'm1', type: 'line', title: 't', data: { source: 'url', shape: 'series' } }],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/url/);
  });

  it('rejects url data with bad shape', () => {
    const r = validateManifest({
      ...base,
      marks: [{ id: 'm1', type: 'line', title: 't', data: { source: 'url', url: '/x', shape: 'pancake' } }],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/shape/);
  });

  it('rejects negative refreshInterval', () => {
    const r = validateManifest({
      ...base,
      marks: [{ id: 'm1', type: 'line', title: 't', data: { source: 'url', url: '/x', shape: 'series', refreshInterval: -5 } }],
    });
    expect(r.valid).toBe(false);
  });

  it('accepts a minimal inline tree mark', () => {
    const r = validateManifest({
      ...base,
      marks: [{ id: 'm1', type: 'tree', title: 't', data: { source: 'inline', hierarchy: { name: 'root' } } }],
    });
    expect(r.valid).toBe(true);
  });

  it('accepts a streamgraph mark with distributions data and config.categories', () => {
    const r = validateManifest({
      ...base,
      marks: [{
        id: 'm1', type: 'streamgraph', title: 'cpu load',
        data: { source: 'inline', distributions: [[1, 2, 3], [4, 3, 2]] },
        config: { categories: ['user', 'system'], windowSize: 60 },
      }],
    });
    expect(r.valid).toBe(true);
  });

  it('reports id="..." in error messages for diagnostics', () => {
    const r = validateManifest({
      ...base,
      marks: [{ id: 'borked', type: 'nope', title: 't', data: { source: 'inline' } }],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/borked/);
  });

  it('accumulates multiple errors', () => {
    const r = validateManifest({
      version: '1',
      name: 'x',
      scaleTag: 'galaxy',
      marks: [
        { id: 'a', type: 'unknown1', title: 't', data: { source: 'inline' } },
        { id: 'b', type: 'unknown2', title: 't', data: { source: 'inline' } },
      ],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.length).toBeGreaterThanOrEqual(3);
  });
});

describe('validateManifest — UDM-MagNET v1.0 cross-refs', () => {
  const base = { version: '1' as const, name: 'x', scaleTag: 'room' as const };

  it('accepts a manifest with valid deviceRef and serviceRef', () => {
    const r = validateManifest({
      ...base,
      udm_version: '1.0',
      udm_devices: [{ udm_key: 'dev-a' }],
      usm_services: [{ usm_key: 'svc-a', usm_service_name: 'A' }],
      marks: [{
        id: 'm1', type: 'tree', title: 't',
        data: { source: 'inline', hierarchy: { name: 'root' } },
        deviceRef: 'dev-a', serviceRef: 'svc-a',
      }],
    });
    expect(r.valid).toBe(true);
  });

  it('rejects a deviceRef that does not match any udm_devices[].udm_key', () => {
    const r = validateManifest({
      ...base,
      udm_devices: [{ udm_key: 'dev-a' }],
      marks: [{
        id: 'm1', type: 'tree', title: 't',
        data: { source: 'inline', hierarchy: { name: 'root' } },
        deviceRef: 'dev-ghost',
      }],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/dev-ghost/);
  });

  it('rejects a serviceRef that does not match any usm_services[].usm_key', () => {
    const r = validateManifest({
      ...base,
      usm_services: [{ usm_key: 'svc-a', usm_service_name: 'A' }],
      marks: [{
        id: 'm1', type: 'tree', title: 't',
        data: { source: 'inline', hierarchy: { name: 'root' } },
        serviceRef: 'svc-ghost',
      }],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/svc-ghost/);
  });

  it('skips deviceRef cross-ref check when udm_devices is absent', () => {
    // If a manifest has no udm_devices, deviceRefs are informational only.
    const r = validateManifest({
      ...base,
      marks: [{
        id: 'm1', type: 'tree', title: 't',
        data: { source: 'inline', hierarchy: { name: 'root' } },
        deviceRef: 'something-not-defined',
      }],
    });
    expect(r.valid).toBe(true);
  });

  it('rejects duplicate udm_key in udm_devices', () => {
    const r = validateManifest({
      ...base,
      udm_devices: [{ udm_key: 'dup' }, { udm_key: 'dup' }],
      marks: [],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/duplicated/);
  });

  it('rejects duplicate usm_key in usm_services', () => {
    const r = validateManifest({
      ...base,
      usm_services: [
        { usm_key: 'dup', usm_service_name: 'A' },
        { usm_key: 'dup', usm_service_name: 'B' },
      ],
      marks: [],
    });
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.errors.join(' ')).toMatch(/duplicated/);
  });
});

describe('validateManifest — UDM-MagNET v1.0 warnings', () => {
  const base = { version: '1' as const, name: 'x', scaleTag: 'room' as const };

  it('warns on deprecated udm_oem when udm_vendor is absent', () => {
    const r = validateManifest({
      ...base,
      udm_devices: [{ udm_key: 'dev-a', udm_oem: 'Acme' }],
      marks: [],
    });
    expect(r.valid).toBe(true);
    if (r.valid) {
      expect(r.warnings).toBeDefined();
      expect(r.warnings!.join(' ')).toMatch(/udm_oem/);
    }
  });

  it('does not warn on udm_oem when udm_vendor is also present', () => {
    const r = validateManifest({
      ...base,
      udm_devices: [{ udm_key: 'dev-a', udm_oem: 'Acme', udm_vendor: 'Acme' }],
      marks: [],
    });
    expect(r.valid).toBe(true);
    if (r.valid) {
      const ws = r.warnings ?? [];
      expect(ws.some(w => /udm_oem/.test(w))).toBe(false);
    }
  });

  it('warns on upstream "udm_chipset_vendor" stutter form inside udm_chipset_details', () => {
    const r = validateManifest({
      ...base,
      udm_devices: [{
        udm_key: 'dev-a',
        udm_chipset_details: { udm_chipset_vendor: 'Espressif', udm_cpu_max_frequency: '240MHz' },
      }],
      marks: [],
    });
    expect(r.valid).toBe(true);
    if (r.valid) {
      expect(r.warnings).toBeDefined();
      expect(r.warnings!.join(' ')).toMatch(/stutter/);
    }
  });

  it('warns when udm_version is set to a non-1.0 string', () => {
    const r = validateManifest({
      ...base,
      udm_version: '0.9.1',
      marks: [],
    });
    expect(r.valid).toBe(true);
    if (r.valid) {
      expect(r.warnings).toBeDefined();
      expect(r.warnings!.join(' ')).toMatch(/udm_version/);
    }
  });
});

describe('validateManifest — UDM-MagNET v1.0 underscore annotations', () => {
  const base = { version: '1' as const, name: 'x', scaleTag: 'room' as const };

  it('accepts _comment / _doc keys at any object level', () => {
    const r = validateManifest({
      ...base,
      _comment: 'top-level note',
      udm_devices: [{
        udm_key: 'dev-a',
        _comment: 'device note',
        _doc: 'https://example.com/dev-a',
        udm_chipset_details: { vendor: 'Espressif', _comment_internal: 'verified' },
      }],
      marks: [{
        id: 'm1', type: 'tree', title: 't',
        _comment: 'this mark is the device tree',
        data: { source: 'inline', hierarchy: { name: 'root', _comment: 'root node' } },
      }],
    });
    expect(r.valid).toBe(true);
  });
});
