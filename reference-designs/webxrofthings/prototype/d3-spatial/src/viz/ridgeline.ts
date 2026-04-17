import * as THREE from 'three';

export interface RidgelineOptions {
  width?: number;
  rowHeight?: number;
  depthStep?: number;
  palette?: number[];
}

export function buildRidgeline(series: number[][], opts: RidgelineOptions = {}): THREE.Group {
  const {
    width = 0.3,
    rowHeight = 0.04,
    depthStep = 0.02,
    palette = [0xff5577, 0xff99cc, 0xcc99ff, 0x66ccff, 0x66ffcc, 0xffcc66],
  } = opts;

  const g = new THREE.Group();
  g.name = 'ridgeline';

  const maxV = Math.max(...series.flat());
  const rows = series.length;

  series.forEach((row, rIdx) => {
    const color = palette[rIdx % palette.length]!;
    const pts: THREE.Vector3[] = [];
    const z = -rIdx * depthStep;
    const y0 = rows * 0.012 - rIdx * 0.02;

    row.forEach((v, i) => {
      const x = (i / (row.length - 1)) * width - width / 2;
      const y = y0 + (v / maxV) * rowHeight;
      pts.push(new THREE.Vector3(x, y, z));
    });

    const shape = new THREE.Shape();
    shape.moveTo(pts[0]!.x, 0);
    pts.forEach(p => shape.lineTo(p.x, p.y - y0));
    shape.lineTo(pts[pts.length - 1]!.x, 0);
    shape.closePath();

    const geo = new THREE.ShapeGeometry(shape);
    geo.translate(0, y0, z);
    const mat = new THREE.MeshStandardMaterial({
      color, emissive: color, emissiveIntensity: 0.35,
      roughness: 0.5, metalness: 0.05,
      transparent: true, opacity: 0.85,
      side: THREE.DoubleSide,
    });
    g.add(new THREE.Mesh(geo, mat));

    const curveGeo = new THREE.BufferGeometry().setFromPoints(pts);
    const line = new THREE.Line(
      curveGeo,
      new THREE.LineBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.9 }),
    );
    g.add(line);
  });

  return g;
}
