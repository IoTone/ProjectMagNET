/**
 * Dataspace Manifest Schema v1
 *
 * A dataspace publishes this JSON to describe the marks, data, and interaction
 * capabilities it offers. The d3-spatial renderer loads the manifest and
 * instantiates the appropriate viz builders without per-dataset code changes.
 *
 * UDM/USM fields conform to UDM-MagNET v1.0 / USM-MagNET v1.0
 * (../../../../specs/UDM-MagNET-v1.md), forked from IoTone UDM/USM 0.9.1.
 *
 * See XR_UX-proposal1.md §9, §10 (R19, R28).
 */

/** Top-level manifest published by a dataspace. */
export interface DataspaceManifest {
  /** Schema version. Always "1" for this iteration. */
  version: '1';

  /** Human-readable name of the dataspace. */
  name: string;

  /** Scale tag drives default placement radius from the reader. */
  scaleTag: 'personal' | 'room' | 'hall' | 'net';

  /** Dataspace owner identity (opaque string — email, DID, device ID). */
  owner?: string;

  /** Optional ambient audio bed (ambix-format .ogg). User must opt in. */
  ambisonicBedUrl?: string;

  /** Optional acoustic environment hint for reverb. */
  acousticEnvironment?: 'indoor' | 'outdoor' | 'auto';

  /** The marks this dataspace wants rendered. */
  marks: MarkSpec[];

  /** Optional join-code display configuration (§2 onboarding). */
  joinCode?: {
    rotationSeconds: number;
    charSet: 'alphanumeric-unambiguous';
    length: number;
  };

  /** Optional HUD menu configuration for this dataspace. */
  hud?: DataspaceHudConfig;

  /**
   * UDM-MagNET spec version this document conforms to. Recommended for any
   * manifest that includes `udm_devices` or `usm_services`. See specs/UDM-MagNET-v1.md.
   */
  udm_version?: '1.0';

  /** Author-managed document revision (semver). Optional. */
  udm_doc_version?: string;

  /**
   * Universal Device Metadata entries for devices present in this dataspace.
   * Conforms to UDM-MagNET v1.0 (specs/UDM-MagNET-v1.md).
   * Renderer may surface device info in inspector cards or a "devices" sub-menu.
   */
  udm_devices?: UdmDevice[];

  /**
   * Universal Service Metadata entries for services exposed by devices.
   * Conforms to USM-MagNET v1.0 (specs/UDM-MagNET-v1.md §4).
   * Marks may reference these via `serviceRef: usm_key` for richer metadata.
   */
  usm_services?: UsmService[];
}

/** UDM-MagNET v1.0 device record. See specs/UDM-MagNET-v1.md §3. */
export interface UdmDevice {
  udm_key: string;
  udm_uuid?: string;
  udm_serialno?: string;
  udm_model_name?: string;
  udm_model_number?: string;
  udm_model_rev?: string;
  udm_vendor?: string;
  /** @deprecated alias for udm_vendor; readers SHOULD accept, writers SHOULD emit udm_vendor. */
  udm_oem?: string;
  udm_mktg_name?: string;
  udm_type?: string;
  udm_class?: string[];
  udm_capabilities?: string[];
  udm_chipset_details?: { vendor?: string; type?: string; instruction_set?: string; frequency?: string; core_count?: number };
  udm_memory_volatile?: string | { size?: string; type?: string; extras?: string };
  udm_memory_non_volatile?: string | { size?: string; type?: string; extras?: string };
  udm_sensors?: string[];
  udm_services_link?: string[];  // bare usm_key (in-doc) or URI (external)
  udm_tags?: string[];
  udm_mfg_origin?: string;
  udm_mfg_date?: string;
  /** Spatial-render hint: where the device pin appears in 3D space relative to the dataspace anchor. Extension; promotion candidate for v1.1. */
  udm_spatial_anchor_x?: { x: number; y: number; z: number };
  /** Currently-running firmware version. Extension; promotion candidate for v1.1. */
  udm_fw_version_x?: string;
  [key: string]: unknown;
}

/** Universal Service Metadata — abbreviated form. */
export interface UsmService {
  usm_key: string;
  usm_uuid?: string;
  usm_service_name: string;
  usm_type?: string;
  usm_class?: string[];
  usm_service_endpoint?: { GET?: unknown; POST?: unknown; PUT?: unknown; DELETE?: unknown; [k: string]: unknown };
  usm_version?: string;
  usm_service_version?: string;
  usm_vendor?: string;
  usm_capabilities?: unknown;
  characteristics?: Array<{
    usm_characteristic_id: string;
    usm_characteristic_format?: string;
    usm_characteristic_constraints?: unknown;
    [k: string]: unknown;
  }>;
  usm_tags?: string[];
  [key: string]: unknown;
}

/** Configuration for a per-dataspace HUD context menu. */
export interface DataspaceHudConfig {
  items: DataspaceHudItem[];
  position?: 'bottom' | 'side' | 'wrist';
}

/** A single item in the dataspace HUD context menu. */
export interface DataspaceHudItem {
  id: string;
  label: string;
  icon?: string;  // emoji or text icon
  action: DataspaceHudAction;
}

/** Built-in dataspace HUD actions. Custom string actions are also allowed. */
export type DataspaceHudAction =
  | 'reload-marks'      // re-fetch manifest and rebuild marks
  | 'toggle-ambient'    // toggle ambient audio bed
  | 'show-join-code'    // show the join code for sharing
  | 'leave-dataspace'   // disconnect and return to join panel
  | 'recenter'          // recenter the dataspace anchor
  | 'toggle-labels'     // show/hide node labels
  | 'reset-view'        // reset drill-in state + selections
  | string;             // custom actions (extensible)

/** Default HUD items used when the manifest doesn't specify a hud field. */
export const DEFAULT_HUD_ITEMS: DataspaceHudItem[] = [
  { id: 'recenter', label: 'Recenter', icon: '\u2295', action: 'recenter' },
  { id: 'reset', label: 'Reset', icon: '\u21BA', action: 'reset-view' },
  { id: 'leave', label: 'Leave', icon: '\u2715', action: 'leave-dataspace' },
];

/** A single mark (visualization) within a dataspace manifest. */
export interface MarkSpec {
  /** Unique id within this manifest. */
  id: string;

  /** Mark type — must match a registered builder. */
  type: MarkType;

  /** Human-readable title shown above the mark. */
  title: string;

  /** Optional subtitle shown below the mark. */
  subtitle?: string;

  /** Data source — inline or URL. */
  data: InlineData | UrlData;

  /** Mark-specific configuration. */
  config?: Record<string, unknown>;

  /** Whether this mark supports drill-in (hierarchy marks). */
  drillable?: boolean;

  /** Whether nodes are individually hoverable. */
  hoverable?: boolean;

  /** Whether nodes support drag interaction (force graph). */
  draggable?: boolean;

  /** Optional reference to a USM service (matches `usm_services[].usm_key`). */
  serviceRef?: string;

  /** Optional reference to a UDM device (matches `udm_devices[].udm_key`). */
  deviceRef?: string;
}

/** All supported mark types. */
export type MarkType =
  | 'line' | 'bar' | 'scatter' | 'arc'
  | 'tree' | 'treemap' | 'sunburst' | 'pack'
  | 'force' | 'ridgeline' | 'sankey' | 'streamgraph'
  | 'parallel' | 'tangled-tree' | 'edge-bundle' | 'hexbin'
  | 'video';

export const MARK_TYPES: readonly MarkType[] = [
  'line', 'bar', 'scatter', 'arc',
  'tree', 'treemap', 'sunburst', 'pack',
  'force', 'ridgeline', 'sankey', 'streamgraph',
  'parallel', 'tangled-tree', 'edge-bundle', 'hexbin',
  'video',
];

export const SCALE_TAGS = ['personal', 'room', 'hall', 'net'] as const;
export const URL_DATA_SHAPES = ['hierarchy', 'graph', 'series', 'distributions', 'flow', 'video'] as const;

export type ManifestValidationResult =
  | { valid: true; warnings?: string[] }
  | { valid: false; errors: string[]; warnings?: string[] };

/**
 * Runtime validator for an unknown JSON value claiming to be a DataspaceManifest.
 * Catches the common "shape is wrong" failures that TS types can't enforce at runtime.
 *
 * Underscore-prefixed keys (e.g. `_comment`, `_doc`) are ignorable annotations
 * per UDM-MagNET v1.0 §2.2 and are silently accepted.
 */
export function validateManifest(input: unknown): ManifestValidationResult {
  const errors: string[] = [];
  const warnings: string[] = [];
  const push = (msg: string) => errors.push(msg);
  const warn = (msg: string) => warnings.push(msg);

  if (input === null || typeof input !== 'object') {
    return { valid: false, errors: ['manifest must be an object'] };
  }
  const m = input as Record<string, unknown>;

  if (m.version !== '1') push(`version must be "1" (got ${JSON.stringify(m.version)})`);
  if (typeof m.name !== 'string' || m.name.length === 0) push('name must be a non-empty string');
  if (!SCALE_TAGS.includes(m.scaleTag as typeof SCALE_TAGS[number])) {
    push(`scaleTag must be one of ${SCALE_TAGS.join(', ')} (got ${JSON.stringify(m.scaleTag)})`);
  }
  if (m.udm_version !== undefined && m.udm_version !== '1.0') {
    warn(`udm_version "${m.udm_version}" is not "1.0"; spec divergence may exist`);
  }
  if (!Array.isArray(m.marks)) {
    push('marks must be an array');
    return { valid: false, errors, ...(warnings.length ? { warnings } : {}) };
  }

  // Build deviceKeys / serviceKeys for cross-reference resolution and uniqueness checks.
  const deviceKeys = new Set<string>();
  const serviceKeys = new Set<string>();

  if (Array.isArray(m.udm_devices)) {
    m.udm_devices.forEach((rawDev, idx) => {
      if (rawDev === null || typeof rawDev !== 'object') {
        push(`udm_devices[${idx}] must be an object`);
        return;
      }
      const dev = rawDev as Record<string, unknown>;
      if (typeof dev.udm_key !== 'string' || dev.udm_key.length === 0) {
        push(`udm_devices[${idx}].udm_key must be a non-empty string`);
        return;
      }
      if (deviceKeys.has(dev.udm_key)) {
        push(`udm_devices[${idx}].udm_key "${dev.udm_key}" is duplicated`);
      } else {
        deviceKeys.add(dev.udm_key);
      }
      if (typeof dev.udm_oem === 'string' && typeof dev.udm_vendor !== 'string') {
        warn(`udm_devices[${idx}] uses deprecated udm_oem; prefer udm_vendor (UDM-MagNET v1.0 §8)`);
      }
      // Stutter form inside chipset_details (upstream-style)
      const cs = dev.udm_chipset_details;
      if (cs && typeof cs === 'object') {
        const csObj = cs as Record<string, unknown>;
        for (const stutter of ['udm_chipset_vendor', 'udm_chipset_type', 'udm_chipset_inst_set', 'udm_cpu_max_frequency', 'udm_cpu_number_of_cores']) {
          if (stutter in csObj) {
            warn(`udm_devices[${idx}].udm_chipset_details uses upstream stutter form "${stutter}"; rename per UDM-MagNET v1.0 §3.3`);
          }
        }
      }
    });
  }

  if (Array.isArray(m.usm_services)) {
    m.usm_services.forEach((rawSvc, idx) => {
      if (rawSvc === null || typeof rawSvc !== 'object') {
        push(`usm_services[${idx}] must be an object`);
        return;
      }
      const svc = rawSvc as Record<string, unknown>;
      if (typeof svc.usm_key !== 'string' || svc.usm_key.length === 0) {
        push(`usm_services[${idx}].usm_key must be a non-empty string`);
        return;
      }
      if (serviceKeys.has(svc.usm_key)) {
        push(`usm_services[${idx}].usm_key "${svc.usm_key}" is duplicated`);
      } else {
        serviceKeys.add(svc.usm_key);
      }
      if (typeof svc.usm_service_name !== 'string') {
        push(`usm_services[${idx}] (key="${svc.usm_key}").usm_service_name must be a string`);
      }
    });
  }

  m.marks.forEach((rawMark, idx) => {
    if (rawMark === null || typeof rawMark !== 'object') {
      push(`marks[${idx}] must be an object`);
      return;
    }
    const mark = rawMark as Record<string, unknown>;
    const ref = `marks[${idx}]${typeof mark.id === 'string' ? ` (id="${mark.id}")` : ''}`;

    if (typeof mark.id !== 'string' || mark.id.length === 0) push(`${ref}.id must be a non-empty string`);
    if (typeof mark.title !== 'string') push(`${ref}.title must be a string`);
    if (!MARK_TYPES.includes(mark.type as MarkType)) {
      push(`${ref}.type must be one of the registered mark types (got ${JSON.stringify(mark.type)})`);
    }

    if (mark.data === null || typeof mark.data !== 'object') {
      push(`${ref}.data must be an object`);
      return;
    }
    const data = mark.data as Record<string, unknown>;
    if (data.source === 'inline') {
      // No required field on InlineData beyond `source` — type-specific fields are validated by builders.
    } else if (data.source === 'url') {
      if (typeof data.url !== 'string' || data.url.length === 0) {
        push(`${ref}.data.url must be a non-empty string`);
      }
      if (!URL_DATA_SHAPES.includes(data.shape as typeof URL_DATA_SHAPES[number])) {
        push(`${ref}.data.shape must be one of ${URL_DATA_SHAPES.join(', ')} (got ${JSON.stringify(data.shape)})`);
      }
      if (data.refreshInterval !== undefined && (typeof data.refreshInterval !== 'number' || data.refreshInterval < 0)) {
        push(`${ref}.data.refreshInterval must be a non-negative number`);
      }
    } else {
      push(`${ref}.data.source must be "inline" or "url" (got ${JSON.stringify(data.source)})`);
    }

    // Cross-ref: deviceRef / serviceRef must resolve when udm_devices / usm_services are present.
    if (typeof mark.deviceRef === 'string' && deviceKeys.size > 0 && !deviceKeys.has(mark.deviceRef)) {
      push(`${ref}.deviceRef "${mark.deviceRef}" does not match any udm_devices[].udm_key`);
    }
    if (typeof mark.serviceRef === 'string' && serviceKeys.size > 0 && !serviceKeys.has(mark.serviceRef)) {
      push(`${ref}.serviceRef "${mark.serviceRef}" does not match any usm_services[].usm_key`);
    }
  });

  if (errors.length === 0) {
    return warnings.length ? { valid: true, warnings } : { valid: true };
  }
  return warnings.length ? { valid: false, errors, warnings } : { valid: false, errors };
}

/** Inline data embedded in the manifest. */
export interface InlineData {
  source: 'inline';
  /** For hierarchy marks: HNode tree. */
  hierarchy?: HierarchyData;
  /** For graph marks: nodes + links. */
  graph?: GraphDataSpec;
  /** For time-series marks: array of {t, v} or similar. */
  series?: SeriesData[];
  /** For distribution marks: array of number arrays (rows). */
  distributions?: number[][];
  /** For flow marks (sankey): nodes + links with values. */
  flow?: FlowDataSpec;
}

/** URL-referenced data fetched at load time. */
export interface UrlData {
  source: 'url';
  url: string;
  /** Expected shape — renderer validates after fetch. `video` is a sentinel for binary image/stream URLs. */
  shape: 'hierarchy' | 'graph' | 'series' | 'distributions' | 'flow' | 'video';
  /** Refresh interval in seconds. 0 = one-shot. */
  refreshInterval?: number;
}

/** Hierarchy data shape (matches HNode from sampleHierarchy.ts). */
export interface HierarchyData {
  name: string;
  value?: number;
  children?: HierarchyData[];
}

/** Graph data shape (matches GraphData from sampleHierarchy.ts). */
export interface GraphDataSpec {
  nodes: Array<{ id: string; group?: number; [key: string]: unknown }>;
  links: Array<{ source: string; target: string; value?: number }>;
}

/** Time-series data point. */
export interface SeriesData {
  t: number;
  v: number;
  [key: string]: unknown;
}

/** Flow (Sankey) data shape. */
export interface FlowDataSpec {
  nodes: Array<{ id: string; name: string; group?: number }>;
  links: Array<{ source: string; target: string; value: number }>;
}

// ─── Example manifest ───────────────────────────────────────────────

export const EXAMPLE_MANIFEST: DataspaceManifest = {
  version: '1',
  name: 'kords-livingroom',
  scaleTag: 'room',
  owner: 'dkords@gmail.com',
  marks: [
    {
      id: 'device-tree',
      type: 'tree',
      title: 'device topology',
      data: {
        source: 'inline',
        hierarchy: {
          name: 'root',
          children: [
            { name: 'sensors', children: [
              { name: 'temp', value: 12 },
              { name: 'humidity', value: 8 },
            ]},
            { name: 'actuators', children: [
              { name: 'lights', value: 20 },
              { name: 'hvac', value: 24 },
            ]},
          ],
        },
      },
      config: { form: 'radial' },
      drillable: true,
      hoverable: true,
    },
    {
      id: 'energy-flow',
      type: 'sankey',
      title: 'energy flow',
      data: {
        source: 'inline',
        flow: {
          nodes: [
            { id: 'solar', name: 'Solar', group: 0 },
            { id: 'grid', name: 'Grid', group: 0 },
            { id: 'home', name: 'Home', group: 1 },
            { id: 'lights', name: 'Lights', group: 2 },
            { id: 'hvac', name: 'HVAC', group: 2 },
          ],
          links: [
            { source: 'solar', target: 'home', value: 60 },
            { source: 'grid', target: 'home', value: 40 },
            { source: 'home', target: 'lights', value: 30 },
            { source: 'home', target: 'hvac', value: 70 },
          ],
        },
      },
      hoverable: true,
    },
    {
      id: 'heart-rate',
      type: 'line',
      title: 'HR · last 60 min',
      data: {
        source: 'url',
        url: 'wss://hlxr.org/ds/kords-livingroom/hr',
        shape: 'series',
        refreshInterval: 5,
      },
      hoverable: true,
    },
  ],
};
