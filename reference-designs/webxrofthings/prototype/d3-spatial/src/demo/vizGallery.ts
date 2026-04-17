import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { buildTree, TreeViz } from '../viz/tree';
import { buildTreemap, TreemapViz } from '../viz/treemap';
import { buildSunburst, SunburstViz } from '../viz/sunburst';
import { buildCircularPack, PackViz } from '../viz/pack';
import { buildForceGraph, ForceViz } from '../viz/force';
import { buildRidgeline } from '../viz/ridgeline';
import { sampleTree, sampleGraph, sampleRidgeline } from './sampleHierarchy';
import { TEXT } from '../ui/palette';

export interface GalleryItem {
  id: string;
  title: string;
  group: THREE.Group;
  worldPos: THREE.Vector3;
}

export interface GalleryResult {
  root: THREE.Group;
  items: GalleryItem[];
  force: ForceViz;
  forceCell: THREE.Group;
  tree: TreeViz;
  treeCell: THREE.Group;
  treemap: TreemapViz;
  treemapCell: THREE.Group;
  sunburst: SunburstViz;
  sunburstCell: THREE.Group;
  pack: PackViz;
  packCell: THREE.Group;
}

export function buildVizGallery(): GalleryResult {
  const root = new THREE.Group();
  root.name = 'viz-gallery';

  const items: GalleryItem[] = [];
  const force = buildForceGraph(sampleGraph(28));
  const tree = buildTree(sampleTree, { form: 'radial' });
  const treemap = buildTreemap(sampleTree);
  const sunburst = buildSunburst(sampleTree);
  const pack = buildCircularPack(sampleTree);

  const specs = [
    { id: 'tree',       title: 'tree \u00b7 radial',          viz: tree.group,                                       sublabel: '\u00a79.1 hierarchy \u00b7 node-link' },
    { id: 'treemap',    title: 'treemap \u00b7 extruded',     viz: treemap.group,                                    sublabel: '\u00a79.2 hierarchy \u00b7 area + z' },
    { id: 'sunburst',   title: 'sunburst \u00b7 stacked',     viz: sunburst.group,                                   sublabel: '\u00a79.5 hierarchy \u00b7 radial partition' },
    { id: 'pack',       title: 'circular packing',       viz: pack.group,                                        sublabel: '\u00a79.9 \u2605 nested spheres' },
    { id: 'force',      title: 'force \u00b7 d3-force-3d',    viz: force.group,                                      sublabel: '\u00a79.6 graph \u00b7 3D physics' },
    { id: 'ridgeline',  title: 'ridgeline \u00b7 depth-offset', viz: buildRidgeline(sampleRidgeline(6)),              sublabel: '\u00a79.9 \u2605 distribution' },
  ];

  const cols = 3;
  const rows = Math.ceil(specs.length / cols);
  const cellW = 0.42;
  const cellH = 0.34;

  specs.forEach((s, i) => {
    const col = i % cols;
    const row = Math.floor(i / cols);
    const x = (col - (cols - 1) / 2) * cellW;
    const y = ((rows - 1) / 2 - row) * cellH;

    const cell = new THREE.Group();
    cell.position.set(x, y, 0);
    cell.add(s.viz);

    const title = new Text();
    title.text = s.title;
    title.fontSize = 0.018;
    title.color = TEXT.primary;
    title.anchorX = 'center';
    title.anchorY = 'bottom';
    title.position.set(0, cellH / 2 - 0.02, 0);
    title.sync();
    cell.add(title);

    const sub = new Text();
    sub.text = s.sublabel;
    sub.fontSize = 0.011;
    sub.color = TEXT.muted;
    sub.anchorX = 'center';
    sub.anchorY = 'top';
    sub.position.set(0, -cellH / 2 + 0.015, 0);
    sub.sync();
    cell.add(sub);

    root.add(cell);
    items.push({ id: s.id, title: s.title, group: cell, worldPos: new THREE.Vector3(x, y, 0) });
  });

  const forceCell = items.find(i => i.id === 'force')!.group;
  const treeCell = items.find(i => i.id === 'tree')!.group;
  const treemapCell = items.find(i => i.id === 'treemap')!.group;
  const sunburstCell = items.find(i => i.id === 'sunburst')!.group;
  const packCell = items.find(i => i.id === 'pack')!.group;
  return { root, items, force, forceCell, tree, treeCell, treemap, treemapCell, sunburst, sunburstCell, pack, packCell };
}
