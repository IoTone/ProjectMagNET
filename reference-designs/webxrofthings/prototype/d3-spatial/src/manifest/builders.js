/**
 * Mark builder registrations — bridges manifest schema to viz builders.
 *
 * Each builder takes a MarkSpec and returns a LoadedMark ready for the scene.
 * Call registerAllBuilders() once at startup.
 */
import { registerMarkBuilder, extractHierarchy, extractGraph, extractFlow, extractDistributions } from './loader';
import { buildTree } from '../viz/tree';
import { buildTreemap } from '../viz/treemap';
import { buildSunburst } from '../viz/sunburst';
import { buildCircularPack } from '../viz/pack';
import { buildForceGraph } from '../viz/force';
import { buildRidgeline } from '../viz/ridgeline';
import { buildSankey } from '../viz/sankey';
import { buildVideoPanel } from '../viz/videoPanel';
function makeMark(spec, group, viz, defaults) {
    return {
        id: spec.id,
        type: spec.type,
        title: spec.title,
        subtitle: spec.subtitle,
        group,
        viz,
        drillable: spec.drillable ?? defaults?.drillable ?? false,
        hoverable: spec.hoverable ?? defaults?.hoverable ?? false,
        draggable: spec.draggable ?? defaults?.draggable ?? false,
    };
}
export function registerAllBuilders() {
    registerMarkBuilder('tree', (spec) => {
        const data = extractHierarchy(spec);
        if (!data)
            return null;
        const viz = buildTree(data, { form: spec.config?.form ?? 'radial' });
        return makeMark(spec, viz.group, viz, { drillable: true, hoverable: true });
    });
    registerMarkBuilder('treemap', (spec) => {
        const data = extractHierarchy(spec);
        if (!data)
            return null;
        const viz = buildTreemap(data);
        return makeMark(spec, viz.group, viz, { drillable: true, hoverable: true });
    });
    registerMarkBuilder('sunburst', (spec) => {
        const data = extractHierarchy(spec);
        if (!data)
            return null;
        const viz = buildSunburst(data);
        return makeMark(spec, viz.group, viz, { drillable: true, hoverable: true });
    });
    registerMarkBuilder('pack', (spec) => {
        const data = extractHierarchy(spec);
        if (!data)
            return null;
        const viz = buildCircularPack(data);
        return makeMark(spec, viz.group, viz, { drillable: true, hoverable: true });
    });
    registerMarkBuilder('force', (spec) => {
        const data = extractGraph(spec);
        if (!data)
            return null;
        const viz = buildForceGraph(data);
        return makeMark(spec, viz.group, viz, { hoverable: true, draggable: true });
    });
    registerMarkBuilder('ridgeline', (spec) => {
        const data = extractDistributions(spec);
        if (!data)
            return null;
        const viz = buildRidgeline(data);
        return makeMark(spec, viz.group, viz);
    });
    registerMarkBuilder('sankey', (spec) => {
        const data = extractFlow(spec);
        if (!data)
            return null;
        const viz = buildSankey(data);
        return makeMark(spec, viz.group, viz, { hoverable: true });
    });
    registerMarkBuilder('video', (spec) => {
        if (spec.data.source !== 'url')
            return null;
        const url = spec.data.url;
        const cfg = (spec.config ?? {});
        const viz = buildVideoPanel({
            url,
            type: cfg.type ?? 'hls',
            width: cfg.width ?? 0.4,
            aspectRatio: cfg.aspectRatio ?? 16 / 9,
            title: spec.title,
            autoplay: cfg.autoplay ?? true,
            muted: cfg.muted ?? true,
        });
        return makeMark(spec, viz.group, viz);
    });
}
