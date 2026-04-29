/**
 * Dataspace Manifest Schema v1
 *
 * A dataspace publishes this JSON to describe the marks, data, and interaction
 * capabilities it offers. The d3-spatial renderer loads the manifest and
 * instantiates the appropriate viz builders without per-dataset code changes.
 *
 * See XR_UX-proposal1.md §9, §10 (R19, R28).
 */
/** Default HUD items used when the manifest doesn't specify a hud field. */
export const DEFAULT_HUD_ITEMS = [
    { id: 'recenter', label: 'Recenter', icon: '\u2295', action: 'recenter' },
    { id: 'reset', label: 'Reset', icon: '\u21BA', action: 'reset-view' },
    { id: 'leave', label: 'Leave', icon: '\u2715', action: 'leave-dataspace' },
];
// ─── Example manifest ───────────────────────────────────────────────
export const EXAMPLE_MANIFEST = {
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
                            ] },
                        { name: 'actuators', children: [
                                { name: 'lights', value: 20 },
                                { name: 'hvac', value: 24 },
                            ] },
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
