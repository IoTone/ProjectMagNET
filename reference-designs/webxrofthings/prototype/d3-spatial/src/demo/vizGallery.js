import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { buildTree } from '../viz/tree';
import { buildTreemap } from '../viz/treemap';
import { buildSunburst } from '../viz/sunburst';
import { buildCircularPack } from '../viz/pack';
import { buildForceGraph } from '../viz/force';
import { buildRidgeline } from '../viz/ridgeline';
import { buildSankey } from '../viz/sankey';
import { buildTidyTree } from '../viz/tidyTree';
import { buildTangledTree } from '../viz/tangledTree';
import { buildParallel } from '../viz/parallel';
import { buildEdgeBundle } from '../viz/edgeBundle';
import { buildMorphDemo } from './morphDemo';
import { buildVideoPanel } from '../viz/videoPanel';
import { sampleTree, sampleGraph, sampleRidgeline, sampleSankey, sampleTangles, sampleParallel } from './sampleHierarchy';
import { TEXT } from '../ui/palette';
export function buildVizGallery() {
    const root = new THREE.Group();
    root.name = 'viz-gallery';
    const items = [];
    const force = buildForceGraph(sampleGraph(28));
    const tree = buildTree(sampleTree, { form: 'radial' });
    const treemap = buildTreemap(sampleTree);
    const sunburst = buildSunburst(sampleTree);
    const pack = buildCircularPack(sampleTree);
    const ridgeline = buildRidgeline(sampleRidgeline(6));
    const sankey = buildSankey(sampleSankey());
    const tidyTree = buildTidyTree(sampleTree);
    const tangledTree = buildTangledTree(sampleTree, sampleTangles());
    const parallel = buildParallel(sampleParallel());
    const edgeBundle = buildEdgeBundle(sampleTree, sampleGraph(28));
    const morphDemo = buildMorphDemo(sampleTree);
    // Camera URL. Default: /camera/capture (served via Vite proxy). This keeps
    // the camera on the LAN and only tunnels the app — cloudflared never sees
    // the camera directly, avoiding header-buffer overruns and connection resets.
    const CAMERA_URL = import.meta.env.VITE_CAMERA_URL ?? '/camera/capture';
    const CAMERA_MODE = import.meta.env.VITE_CAMERA_MODE ?? 'frames';
    const videoPanel = buildVideoPanel({
        url: CAMERA_URL,
        type: CAMERA_MODE,
        width: 0.28,
        aspectRatio: 4 / 3,
        title: 'video · ESP32-CAM',
        frameIntervalMs: 1000,
    });
    const specs = [
        { id: 'tree', title: 'tree \u00b7 radial', viz: tree.group, sublabel: '\u00a79.1 hierarchy \u00b7 node-link' },
        { id: 'treemap', title: 'treemap \u00b7 extruded', viz: treemap.group, sublabel: '\u00a79.2 hierarchy \u00b7 area + z' },
        { id: 'sunburst', title: 'sunburst \u00b7 stacked', viz: sunburst.group, sublabel: '\u00a79.5 hierarchy \u00b7 radial partition' },
        { id: 'pack', title: 'circular packing', viz: pack.group, sublabel: '\u00a79.9 \u2605 nested spheres' },
        { id: 'force', title: 'force \u00b7 d3-force-3d', viz: force.group, sublabel: '\u00a79.6 graph \u00b7 3D physics' },
        { id: 'ridgeline', title: 'ridgeline \u00b7 depth-offset', viz: ridgeline.group, sublabel: '\u00a79.9 \u2605 distribution' },
        { id: 'sankey', title: 'sankey \u00b7 3D tubes', viz: sankey.group, sublabel: '\u00a79.9 \u2605 flow network' },
        { id: 'tidyTree', title: 'tidy tree \u00b7 cylindrical', viz: tidyTree.group, sublabel: '\u00a79 Reingold-Tilford \u00b7 cylinder' },
        { id: 'tangledTree', title: 'tangled tree \u00b7 arcs', viz: tangledTree.group, sublabel: '\u00a79 tree + cross-links' },
        { id: 'parallel', title: 'parallel coords', viz: parallel.group, sublabel: '\u00a79 multivariate axes' },
        { id: 'edgeBundle', title: 'edge bundling', viz: edgeBundle.group, sublabel: '\u00a79 hierarchical routing' },
        { id: 'morph', title: 'morph \u00b7 layout transition', viz: morphDemo.group, sublabel: 'tree \u2192 sunburst \u2192 treemap \u2192 pack' },
        { id: 'video', title: 'video \u00b7 ESP32-CAM', viz: videoPanel.group, sublabel: 'live MJPEG camera feed' },
    ];
    const cols = 4;
    const rows = Math.ceil(specs.length / cols);
    const cellW = 0.38;
    const cellH = 0.30;
    const rowGap = 0.10;
    specs.forEach((s, i) => {
        const col = i % cols;
        const row = Math.floor(i / cols);
        const x = (col - (cols - 1) / 2) * cellW;
        const y = ((rows - 1) / 2 - row) * (cellH + rowGap);
        const cell = new THREE.Group();
        cell.position.set(x, y, 0);
        cell.add(s.viz);
        const title = new Text();
        title.text = s.title;
        title.fontSize = 0.018;
        title.color = TEXT.primary;
        title.anchorX = 'center';
        title.anchorY = 'bottom';
        title.position.set(0, cellH / 2 + 0.015, 0.03);
        title.sync();
        cell.add(title);
        const sub = new Text();
        sub.text = s.sublabel;
        sub.fontSize = 0.011;
        sub.color = TEXT.muted;
        sub.anchorX = 'center';
        sub.anchorY = 'top';
        sub.position.set(0, -cellH / 2 - 0.015, 0.03);
        sub.sync();
        cell.add(sub);
        root.add(cell);
        items.push({ id: s.id, title: s.title, group: cell, worldPos: new THREE.Vector3(x, y, 0) });
    });
    const forceCell = items.find(i => i.id === 'force').group;
    const treeCell = items.find(i => i.id === 'tree').group;
    const treemapCell = items.find(i => i.id === 'treemap').group;
    const sunburstCell = items.find(i => i.id === 'sunburst').group;
    const packCell = items.find(i => i.id === 'pack').group;
    const sankeyCell = items.find(i => i.id === 'sankey').group;
    const tidyTreeCell = items.find(i => i.id === 'tidyTree').group;
    const tangledTreeCell = items.find(i => i.id === 'tangledTree').group;
    const parallelCell = items.find(i => i.id === 'parallel').group;
    const edgeBundleCell = items.find(i => i.id === 'edgeBundle').group;
    const morphCell = items.find(i => i.id === 'morph').group;
    const videoCell = items.find(i => i.id === 'video').group;
    return {
        root, items, force, forceCell, tree, treeCell, treemap, treemapCell,
        sunburst, sunburstCell, pack, packCell, ridgeline, sankey, sankeyCell,
        tidyTree, tidyTreeCell, tangledTree, tangledTreeCell,
        parallel, parallelCell, edgeBundle, edgeBundleCell,
        morphDemo, morphCell,
        videoPanel, videoCell,
    };
}
