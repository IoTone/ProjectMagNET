import * as THREE from 'three';

export interface LineMarkOptions {
  color?: number;
  radius?: number;
  segments?: number;
  tubularSegments?: number;
  curveTension?: number;
}

export function buildLineMark(
  points: THREE.Vector3[],
  opts: LineMarkOptions = {},
): THREE.Group {
  const {
    color = 0x66ffcc,
    radius = 0.004,
    segments = 8,
    tubularSegments,
    curveTension = 0.5,
  } = opts;

  const g = new THREE.Group();
  g.name = 'line-mark';

  if (points.length < 2) return g;

  const curve = new THREE.CatmullRomCurve3(points, false, 'catmullrom', curveTension);
  const tubSeg = tubularSegments ?? Math.max(64, points.length * 2);
  const geo = new THREE.TubeGeometry(curve, tubSeg, radius, segments, false);
  const mat = new THREE.MeshStandardMaterial({
    color,
    roughness: 0.5,
    metalness: 0.1,
    emissive: color,
    emissiveIntensity: 0.25,
  });
  const mesh = new THREE.Mesh(geo, mat);
  g.add(mesh);

  return g;
}
