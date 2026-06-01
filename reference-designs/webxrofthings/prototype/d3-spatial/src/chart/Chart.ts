import * as THREE from 'three';
import { buildLineMark, updateLineMark, LineMarkOptions } from './marks/line';
import { buildBarMark, updateBarMark, BarMarkOptions } from './marks/bar';
import { buildScatterMark, ScatterMarkOptions } from './marks/scatter';
import { buildArcMark, ArcMarkOptions } from './marks/arc';

export type MarkType = 'line' | 'bar' | 'scatter' | 'arc';

export interface ChartDatum {
  [key: string]: unknown;
}

type ScaleFn = ((v: number) => number) & { domain?: unknown; range?: unknown };

export interface ChartOptions {
  xAccessor?: (d: ChartDatum) => number;
  yAccessor?: (d: ChartDatum) => number;
  markOptions?: LineMarkOptions | BarMarkOptions | ScatterMarkOptions | ArcMarkOptions;
  baseline?: number;
  showAxes?: boolean;
}

export interface BrushResult {
  indices: number[];
  count: number;
  xRange: [number, number];
}

export class Chart {
  readonly object3D = new THREE.Group();
  private _x?: ScaleFn;
  private _y?: ScaleFn;
  private _mark: MarkType = 'line';
  private _data: ChartDatum[] = [];
  private _opts: ChartOptions;
  private _marks = new THREE.Group();
  private _axes = new THREE.Group();
  private _brushLayer = new THREE.Group();
  private _brushPlane: THREE.Mesh | null = null;
  private _xRange: [number, number] = [0, 0];

  // Live-update tween state
  private _tweenStart = 0;
  private _tweenDuration = 300; // ms
  private _tweenOldPositions: THREE.Vector3[] = [];
  private _tweenNewPositions: THREE.Vector3[] = [];
  private _tweening = false;

  constructor(opts: ChartOptions = {}) {
    this._opts = opts;
    this.object3D.name = 'd3-spatial.Chart';
    this._marks.name = 'marks';
    this._axes.name = 'axes';
    this._brushLayer.name = 'brush';
    this.object3D.add(this._axes);
    this.object3D.add(this._marks);
    this.object3D.add(this._brushLayer);
  }

  get xScale(): ScaleFn | undefined { return this._x; }
  get yScale(): ScaleFn | undefined { return this._y; }
  get dataset(): ChartDatum[] { return this._data; }
  get markType(): MarkType { return this._mark; }

  x(scale: ScaleFn): this { this._x = scale; return this; }
  y(scale: ScaleFn): this { this._y = scale; return this; }
  mark(type: MarkType): this { this._mark = type; return this; }
  data(arr: ChartDatum[]): this { this._data = arr; return this; }

  render(): this {
    this._marks.clear();
    this._axes.clear();
    if (!this._x || !this._y || this._data.length === 0) return this;

    const xa = this._opts.xAccessor ?? ((d: ChartDatum) => (d as { t: number }).t);
    const ya = this._opts.yAccessor ?? ((d: ChartDatum) => (d as { v: number }).v);

    const points = this._data.map(d => new THREE.Vector3(
      this._x!(xa(d)),
      this._y!(ya(d)),
      0,
    ));

    if (this._mark === 'line') {
      this._marks.add(buildLineMark(points, this._opts.markOptions as LineMarkOptions));
    } else if (this._mark === 'bar') {
      const baseline = this._opts.baseline ?? this._y!((this._y as unknown as { domain: () => [number, number] }).domain()[0]);
      this._marks.add(buildBarMark(points, baseline, this._opts.markOptions as BarMarkOptions));
    } else if (this._mark === 'scatter') {
      this._marks.add(buildScatterMark(points, this._opts.markOptions as ScatterMarkOptions));
    } else if (this._mark === 'arc') {
      this._marks.add(buildArcMark(this._opts.markOptions as ArcMarkOptions));
    }

    if (this._opts.showAxes !== false && this._mark !== 'arc') {
      this._axes.add(buildAxes(this._x, this._y));
    }

    const xDomain = (this._x as unknown as { domain: () => [number, number] }).domain();
    const yDomain = (this._y as unknown as { domain: () => [number, number] }).domain();
    const xPx0 = this._x(xDomain[0]);
    const xPx1 = this._x(xDomain[1]);
    const yPx0 = this._y(yDomain[0]);
    const yPx1 = this._y(yDomain[1]);
    this._xRange = [Math.min(xPx0, xPx1), Math.max(xPx0, xPx1)];
    const w = Math.abs(xPx1 - xPx0);
    const h = Math.abs(yPx1 - yPx0);
    if (this._brushPlane) this.object3D.remove(this._brushPlane);
    if (this._mark !== 'arc' && w > 0 && h > 0) {
      const plane = new THREE.Mesh(
        new THREE.PlaneGeometry(w, h),
        new THREE.MeshBasicMaterial({ visible: false, side: THREE.DoubleSide }),
      );
      plane.position.set((xPx0 + xPx1) / 2, (yPx0 + yPx1) / 2, 0);
      plane.userData.isBrushPlane = true;
      plane.userData.chart = this;
      this._brushPlane = plane;
      this.object3D.add(plane);
    }
    return this;
  }

  /** Convert a point in this chart's local space to a 0..1 x-fraction. */
  xFractionAt(localX: number): number {
    const [lo, hi] = this._xRange;
    if (hi === lo) return 0;
    return Math.max(0, Math.min(1, (localX - lo) / (hi - lo)));
  }

  brush(xFrac0: number, xFrac1: number): BrushResult {
    this.clearBrush();
    if (!this._x || !this._y || this._data.length === 0) {
      return { indices: [], count: 0, xRange: [0, 0] };
    }
    const xa = this._opts.xAccessor ?? ((d: ChartDatum) => (d as { t: number }).t);
    const xDomain = (this._x as unknown as { domain: () => [number, number] }).domain();
    const yDomain = (this._y as unknown as { domain: () => [number, number] }).domain();

    const xPx0 = this._x(xDomain[0]);
    const xPx1 = this._x(xDomain[1]);
    const yPx0 = this._y(yDomain[0]);
    const yPx1 = this._y(yDomain[1]);

    const a = Math.min(xFrac0, xFrac1);
    const b = Math.max(xFrac0, xFrac1);
    const xLo = xPx0 + (xPx1 - xPx0) * a;
    const xHi = xPx0 + (xPx1 - xPx0) * b;
    const yLo = Math.min(yPx0, yPx1);
    const yHi = Math.max(yPx0, yPx1);

    const rectGeo = new THREE.PlaneGeometry(xHi - xLo, yHi - yLo);
    const rectMat = new THREE.MeshBasicMaterial({
      color: 0x66ffff, transparent: true, opacity: 0.18,
      depthTest: false, side: THREE.DoubleSide,
    });
    const rect = new THREE.Mesh(rectGeo, rectMat);
    rect.position.set((xLo + xHi) / 2, (yLo + yHi) / 2, 0.002);
    rect.renderOrder = 990;
    this._brushLayer.add(rect);

    const edges = new THREE.LineSegments(
      new THREE.EdgesGeometry(rectGeo),
      new THREE.LineBasicMaterial({ color: 0x66ffff, transparent: true, opacity: 0.9, depthTest: false }),
    );
    edges.position.copy(rect.position);
    edges.renderOrder = 991;
    this._brushLayer.add(edges);

    const indices: number[] = [];
    for (let i = 0; i < this._data.length; i++) {
      const xVal = xa(this._data[i]!);
      const xPx = this._x(xVal);
      if (xPx >= xLo && xPx <= xHi) indices.push(i);
    }
    return { indices, count: indices.length, xRange: [xLo, xHi] };
  }

  clearBrush(): void {
    while (this._brushLayer.children.length) {
      const c = this._brushLayer.children[0]!;
      const m = c as THREE.Mesh | THREE.LineSegments;
      if (m.geometry) m.geometry.dispose();
      const mat = m.material as THREE.Material | undefined;
      if (mat) mat.dispose();
      this._brushLayer.remove(c);
    }
  }

  /**
   * Update with new data and smoothly tween mark positions.
   * For line marks: rebuilds geometry with crossfade.
   * For bar marks: tweens instance heights.
   * For scatter: rebuilds (no tween — point count may change).
   */
  updateData(newData: ChartDatum[]): void {
    if (!this._x || !this._y || newData.length === 0) return;

    const xa = this._opts.xAccessor ?? ((d: ChartDatum) => (d as { t: number }).t);
    const ya = this._opts.yAccessor ?? ((d: ChartDatum) => (d as { v: number }).v);

    // Recompute scale domains from new data
    const xVals = newData.map(d => xa(d) as number);
    const yVals = newData.map(d => ya(d) as number);
    const xMin = Math.min(...xVals);
    const xMax = Math.max(...xVals);
    const yMin = Math.min(...yVals);
    const yMax = Math.max(...yVals);

    // Update scale domains if the scale has a domain setter
    const xScale = this._x as any;
    const yScale = this._y as any;
    if (xScale.domain && typeof xScale.domain === 'function') {
      xScale.domain([xMin, xMax]);
    }
    if (yScale.domain && typeof yScale.domain === 'function') {
      // Keep the existing visual range stable (don't shrink/grow Y too much)
      const existingYDomain = yScale.domain();
      yScale.domain([
        Math.min(existingYDomain[0], yMin - 5),
        Math.max(existingYDomain[1], yMax + 5),
      ]);
    }

    const newPoints = newData.map(d => new THREE.Vector3(
      this._x!(xa(d) as number),
      this._y!(ya(d) as number),
      0,
    ));

    this._data = newData;

    if (this._mark === 'line') {
      // For line: rebuild the tube geometry within the existing marks group
      const lineGroup = this._marks.children[0] as THREE.Group | undefined;
      if (lineGroup) {
        updateLineMark(lineGroup, newPoints, this._opts.markOptions as LineMarkOptions);
      } else {
        this._marks.clear();
        this._marks.add(buildLineMark(newPoints, this._opts.markOptions as LineMarkOptions));
      }
    } else if (this._mark === 'bar') {
      const barGroup = this._marks.children[0] as THREE.Group | undefined;
      const baseline = this._opts.baseline ?? this._y!((this._y as unknown as { domain: () => [number, number] }).domain()[0]);
      if (barGroup) {
        updateBarMark(barGroup, newPoints, baseline, this._opts.markOptions as BarMarkOptions);
      } else {
        this._marks.clear();
        this._marks.add(buildBarMark(newPoints, baseline, this._opts.markOptions as BarMarkOptions));
      }
    } else {
      // For scatter/arc: full rebuild
      this._marks.clear();
      if (this._mark === 'scatter') {
        this._marks.add(buildScatterMark(newPoints, this._opts.markOptions as ScatterMarkOptions));
      }
    }

    // Rebuild axes and brush plane
    this._axes.clear();
    if (this._opts.showAxes !== false && this._mark !== 'arc') {
      this._axes.add(buildAxes(this._x, this._y));
    }

    const xDomain = (this._x as unknown as { domain: () => [number, number] }).domain();
    const yDomain = (this._y as unknown as { domain: () => [number, number] }).domain();
    const xPx0 = this._x(xDomain[0]);
    const xPx1 = this._x(xDomain[1]);
    const yPx0 = this._y(yDomain[0]);
    const yPx1 = this._y(yDomain[1]);
    this._xRange = [Math.min(xPx0, xPx1), Math.max(xPx0, xPx1)];

    const w = Math.abs(xPx1 - xPx0);
    const h = Math.abs(yPx1 - yPx0);
    if (this._brushPlane) this.object3D.remove(this._brushPlane);
    if (this._mark !== 'arc' && w > 0 && h > 0) {
      const plane = new THREE.Mesh(
        new THREE.PlaneGeometry(w, h),
        new THREE.MeshBasicMaterial({ visible: false, side: THREE.DoubleSide }),
      );
      plane.position.set((xPx0 + xPx1) / 2, (yPx0 + yPx1) / 2, 0);
      plane.userData.isBrushPlane = true;
      plane.userData.chart = this;
      this._brushPlane = plane;
      this.object3D.add(plane);
    }
  }

  dispose(): void {
    this.object3D.traverse(o => {
      const m = o as THREE.Mesh;
      if (m.geometry) m.geometry.dispose();
      const mat = m.material as THREE.Material | THREE.Material[] | undefined;
      if (Array.isArray(mat)) mat.forEach(x => x.dispose());
      else if (mat) mat.dispose();
    });
  }
}

function buildAxes(xScale: ScaleFn, yScale: ScaleFn): THREE.Group {
  const g = new THREE.Group();
  g.name = 'axis-frame';

  const xDomain = (xScale as unknown as { domain: () => [number, number] }).domain();
  const yDomain = (yScale as unknown as { domain: () => [number, number] }).domain();
  const x0 = xScale(xDomain[0]);
  const x1 = xScale(xDomain[1]);
  const y0 = yScale(yDomain[0]);
  const y1 = yScale(yDomain[1]);

  const axisMat = new THREE.LineBasicMaterial({ color: 0x9a8a70, transparent: true, opacity: 0.7 });

  const xAxis = new THREE.Line(
    new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(x0, y0, 0),
      new THREE.Vector3(x1, y0, 0),
    ]),
    axisMat,
  );
  const yAxis = new THREE.Line(
    new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(x0, y0, 0),
      new THREE.Vector3(x0, y1, 0),
    ]),
    axisMat,
  );
  g.add(xAxis);
  g.add(yAxis);
  return g;
}
