/**
 * Dataspace Manifest Schema v1
 *
 * A dataspace publishes this JSON to describe the marks, data, and interaction
 * capabilities it offers. The d3-spatial renderer loads the manifest and
 * instantiates the appropriate viz builders without per-dataset code changes.
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
   * Universal Device Metadata (UDM) entries for devices present in this dataspace.
   * Conforms to https://github.com/IoTone/IoToneSpec_UniversalDeviceMetadata
   * Renderer may surface device info in inspector cards or a "devices" sub-menu.
   */
  udm_devices?: UdmDevice[];

  /**
   * Universal Service Metadata (USM) entries for services exposed by devices.
   * Conforms to https://github.com/IoTone/IoToneSpec_UniversalServiceMetadata
   * Marks may reference these via `serviceRef: usm_key` for richer metadata.
   */
  usm_services?: UsmService[];
}

/** Universal Device Metadata — abbreviated form. Full spec is open-ended. */
export interface UdmDevice {
  udm_key: string;
  udm_uuid?: string;
  udm_serialno?: string;
  udm_model_name?: string;
  udm_model_number?: string;
  udm_vendor?: string;
  udm_mktg_name?: string;
  udm_type?: string;
  udm_class?: string[];
  udm_capabilities?: string[];
  udm_chipset_details?: { vendor?: string; type?: string; frequency?: string; core_count?: number };
  udm_memory_volatile?: string;
  udm_memory_non_volatile?: string;
  udm_sensors?: string[];
  udm_services_link?: string[];  // references to usm_key values
  udm_tags?: string[];
  udm_mfg_origin?: string;
  /** Optional spatial-render hint: where the device pin appears in 3D space */
  udm_spatial_anchor?: { x: number; y: number; z: number };
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
  | 'force' | 'ridgeline' | 'sankey'
  | 'parallel' | 'tangled-tree' | 'edge-bundle' | 'hexbin'
  | 'video';

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
  /** Expected shape — renderer validates after fetch. */
  shape: 'hierarchy' | 'graph' | 'series' | 'distributions' | 'flow';
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
