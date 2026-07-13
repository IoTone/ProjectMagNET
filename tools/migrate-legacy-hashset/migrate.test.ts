import { describe, it, expect } from 'vitest';
import { readFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { parseKvText, migrateLegacyHashSet, type LegacyEndpoint } from './migrate';

const __dirname = dirname(fileURLToPath(import.meta.url));
const FIXTURE = join(__dirname, 'examples', 'zigbee-onoff-switch.kv');

describe('parseKvText', () => {
  it('parses iotonekit K<n>/V<n> paired lines', () => {
    const text = `K1=udm_key\nV1="dev-a"\nK2=udm_oem\nV2="Acme"\n`;
    const pairs = parseKvText(text);
    expect(pairs).toEqual([
      { key: 'udm_key', value: 'dev-a' },
      { key: 'udm_oem', value: 'Acme' },
    ]);
  });

  it('parses simple key=value lines', () => {
    const text = `udm_key=dev-a\nudm_oem="Acme"\n# a comment\n// another\n`;
    const pairs = parseKvText(text);
    expect(pairs).toEqual([
      { key: 'udm_key', value: 'dev-a' },
      { key: 'udm_oem', value: 'Acme' },
    ]);
  });

  it('drops unpaired K entries', () => {
    const text = `K1=udm_key\nV1="dev-a"\nK2=udm_oem\n`;
    const pairs = parseKvText(text);
    expect(pairs).toEqual([{ key: 'udm_key', value: 'dev-a' }]);
  });

  it('handles quoted keys with colons (endpoint-prefix form)', () => {
    const text = `K1="1:udm_capability_x:OnOff"\nV1="0006"\n`;
    const pairs = parseKvText(text);
    expect(pairs).toEqual([{ key: '1:udm_capability_x:OnOff', value: '0006' }]);
  });
});

describe('migrateLegacyHashSet — top-level fields', () => {
  it('always pins udm_version to "1.0"', () => {
    const { device } = migrateLegacyHashSet([
      { key: 'udm_version', value: '0.9.1' },
      { key: 'udm_key', value: 'dev-a' },
    ]);
    expect(device.udm_version).toBe('1.0');
    expect(device.udm_source_version_x).toBe('0.9.1');
  });

  it('renames udm_oem → udm_vendor with a warning', () => {
    const { device, warnings } = migrateLegacyHashSet([{ key: 'udm_oem', value: 'Acme' }]);
    expect(device.udm_vendor).toBe('Acme');
    expect(device.udm_oem).toBeUndefined();
    expect(warnings.join(' ')).toMatch(/udm_oem.*udm_vendor/);
  });

  it('preserves unrenamed top-level fields verbatim', () => {
    const { device } = migrateLegacyHashSet([
      { key: 'udm_key', value: 'dev-a' },
      { key: 'udm_model_name', value: 'Switch' },
      { key: 'udm_serialno', value: 'SN-123' },
    ]);
    expect(device.udm_key).toBe('dev-a');
    expect(device.udm_model_name).toBe('Switch');
    expect(device.udm_serialno).toBe('SN-123');
  });

  it('handles ":_" invocation suffix on top-level keys as <field>_value_x', () => {
    const { device } = migrateLegacyHashSet([
      { key: 'udm_model_name', value: 'IoTone Proto Switch 1' },
      { key: 'udm_model_name:_', value: 'Proto Switch 1' },
    ]);
    expect(device.udm_model_name).toBe('IoTone Proto Switch 1');
    expect(device.udm_model_name_value_x).toBe('Proto Switch 1');
  });

  it('renames "udm_oem:_" companion to "udm_vendor_value_x"', () => {
    const { device } = migrateLegacyHashSet([
      { key: 'udm_oem', value: 'IoTone' },
      { key: 'udm_oem:_', value: 'iotone' },
    ]);
    expect(device.udm_vendor).toBe('IoTone');
    expect(device.udm_vendor_value_x).toBe('iotone');
  });
});

describe('migrateLegacyHashSet — endpoints', () => {
  it('groups endpoint-prefixed keys into udm_endpoints_x', () => {
    const { device, warnings } = migrateLegacyHashSet([
      { key: 'udm_endpoints_x', value: '1' },
      { key: '1:udm_type:OnOff Switch', value: '0103' },
      { key: '1:udm_capability_x:Identify', value: '0003' },
      { key: '1:udm_capability_x:Identify:_', value: '0' },
      { key: '1:udm_capability_x:OnOff', value: '0006' },
      { key: '1:udm_capability_x:OnOff:_', value: '1' },
      { key: '1:udm_capabilities_x', value: 'Identify,OnOff' },
    ]);

    expect(Array.isArray(device.udm_endpoints_x)).toBe(true);
    const eps = device.udm_endpoints_x as LegacyEndpoint[];
    expect(eps).toHaveLength(1);
    const ep = eps[0]!;
    expect(ep.endpoint_index).toBe(1);
    expect(ep.udm_type).toBe('OnOff Switch');
    expect(ep.udm_type_id_x).toBe('0103');
    expect(ep.udm_capabilities_x).toEqual([
      { name: 'Identify', id: '0003', value: '0' },
      { name: 'OnOff',    id: '0006', value: '1' },
    ]);
    expect(warnings.join(' ')).toMatch(/legacy Zigbee-flavored/);
  });

  it('warns when the scalar endpoint count disagrees with the parsed count', () => {
    const { warnings } = migrateLegacyHashSet([
      { key: 'udm_endpoints_x', value: '5' },
      { key: '1:udm_type:Switch', value: '0103' },
    ]);
    expect(warnings.join(' ')).toMatch(/scalar count "5".*1/);
  });
});

describe('migrateLegacyHashSet — chipset stutter', () => {
  it('renames upstream stutter forms inside udm_chipset_details', () => {
    const { device, warnings } = migrateLegacyHashSet([
      { key: 'udm_chipset_details:udm_chipset_vendor', value: 'Espressif' },
      { key: 'udm_chipset_details:udm_cpu_max_frequency', value: '240MHz' },
    ]);
    expect(device.udm_chipset_details).toEqual({ vendor: 'Espressif', frequency: '240MHz' });
    expect(warnings.some(w => /stutter/i.test(w) || /udm_chipset_vendor.*vendor/.test(w))).toBe(true);
  });
});

describe('migrateLegacyHashSet — full iotonekit example', () => {
  it('migrates the documented Zigbee OnOff Switch fixture', () => {
    const text = readFileSync(FIXTURE, 'utf-8');
    const pairs = parseKvText(text);
    expect(pairs.length).toBeGreaterThan(15);

    const { device, warnings } = migrateLegacyHashSet(pairs);

    // Spec compliance
    expect(device.udm_version).toBe('1.0');
    expect(device.udm_source_version_x).toBe('0.9.1');

    // Identity preserved, oem→vendor renamed
    expect(device.udm_key).toBe('iotone-proto-switch-1');
    expect(device.udm_uuid).toBe('1a8748bf-9c13-46ab-a762-e463376a1f27');
    expect(device.udm_model_name).toBe('IoTone Proto Switch 1');
    expect(device.udm_model_number).toBe('IOTONE-SWI-P1');
    expect(device.udm_vendor).toBe('IoTone');
    expect(device.udm_oem).toBeUndefined();
    expect(device.udm_os).toBe('Zigbee HA1.2');

    // Companion ":_" suffix promoted
    expect(device.udm_model_name_value_x).toBe('Proto Switch 1');

    // Endpoint structure
    const eps = device.udm_endpoints_x as LegacyEndpoint[];
    expect(eps).toHaveLength(1);
    expect(eps[0]!.endpoint_index).toBe(1);
    expect(eps[0]!.udm_type).toBe('OnOff Switch');
    expect(eps[0]!.udm_type_id_x).toBe('0103');
    expect(eps[0]!.udm_capabilities_x).toEqual([
      { name: 'Identify', id: '0003', value: '0' },
      { name: 'OnOff',    id: '0006', value: '1' },
    ]);

    // Migration breadcrumb is present and points at the spec
    expect(typeof device._comment_migration).toBe('string');
    expect(device._comment_migration as string).toMatch(/iotonekit/i);

    // We should have at least the rename + zigbee + companion warnings
    expect(warnings.length).toBeGreaterThanOrEqual(3);
  });
});
