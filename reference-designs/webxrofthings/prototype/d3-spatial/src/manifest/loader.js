/**
 * Manifest Loader v1
 *
 * Reads a DataspaceManifest and instantiates viz builders for each mark.
 * This decouples data from rendering — any dataspace that publishes a
 * conformant manifest gets a fully interactive spatial visualization
 * without per-dataset code changes.
 */
const builders = new Map();
/** Register a builder for a mark type. Called once at startup per mark type. */
export function registerMarkBuilder(type, builder) {
    builders.set(type, builder);
}
/** Load a manifest and instantiate all its marks. */
export async function loadManifest(manifest, token) {
    const marks = [];
    for (const spec of manifest.marks) {
        if (spec.data.source === 'url') {
            const urlData = spec.data;
            const url = urlData.url;
            // Handle WebSocket data sources (wss:// or ws://)
            if (url.startsWith('wss://') || url.startsWith('ws://')) {
                // Skip WebSocket sources for now — they require a live server
                console.warn(`[manifest] skipping WebSocket source ${url} (not yet connected)`);
                continue;
            }
            try {
                const headers = {};
                if (token) {
                    headers['Authorization'] = `Bearer ${token}`;
                }
                const resp = await fetch(url, { headers });
                const json = await resp.json();
                spec.data = { source: 'inline', ...shapeToField(urlData.shape, json) };
            }
            catch (e) {
                console.warn(`[manifest] failed to fetch ${url}:`, e);
                continue;
            }
        }
        const builder = builders.get(spec.type);
        if (!builder) {
            console.warn(`[manifest] no builder registered for mark type "${spec.type}"`);
            continue;
        }
        const loaded = builder(spec);
        if (loaded)
            marks.push(loaded);
    }
    return {
        name: manifest.name,
        scaleTag: manifest.scaleTag,
        marks,
    };
}
function shapeToField(shape, data) {
    switch (shape) {
        case 'hierarchy': return { hierarchy: data };
        case 'graph': return { graph: data };
        case 'series': return { series: data };
        case 'distributions': return { distributions: data };
        case 'flow': return { flow: data };
        default: return {};
    }
}
/** Helper: extract hierarchy data from a MarkSpec's inline data. */
export function extractHierarchy(spec) {
    if (spec.data.source !== 'inline')
        return null;
    return spec.data.hierarchy ?? null;
}
/** Helper: extract graph data from a MarkSpec's inline data. */
export function extractGraph(spec) {
    if (spec.data.source !== 'inline')
        return null;
    return spec.data.graph ?? null;
}
/** Helper: extract flow data from a MarkSpec's inline data. */
export function extractFlow(spec) {
    if (spec.data.source !== 'inline')
        return null;
    return spec.data.flow ?? null;
}
/** Helper: extract series data from a MarkSpec's inline data. */
export function extractSeries(spec) {
    if (spec.data.source !== 'inline')
        return null;
    return spec.data.series ?? null;
}
/** Helper: extract distributions data from a MarkSpec's inline data. */
export function extractDistributions(spec) {
    if (spec.data.source !== 'inline')
        return null;
    return spec.data.distributions ?? null;
}
