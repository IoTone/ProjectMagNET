import * as THREE from 'three';
import { buildLineMark, updateLineMark } from './marks/line';
import { buildBarMark, updateBarMark } from './marks/bar';
import { buildScatterMark } from './marks/scatter';
import { buildArcMark } from './marks/arc';
export class Chart {
    object3D = new THREE.Group();
    _x;
    _y;
    _mark = 'line';
    _data = [];
    _opts;
    _marks = new THREE.Group();
    _axes = new THREE.Group();
    _brushLayer = new THREE.Group();
    _brushPlane = null;
    _xRange = [0, 0];
    // Live-update tween state
    _tweenStart = 0;
    _tweenDuration = 300; // ms
    _tweenOldPositions = [];
    _tweenNewPositions = [];
    _tweening = false;
    constructor(opts = {}) {
        this._opts = opts;
        this.object3D.name = 'd3-spatial.Chart';
        this._marks.name = 'marks';
        this._axes.name = 'axes';
        this._brushLayer.name = 'brush';
        this.object3D.add(this._axes);
        this.object3D.add(this._marks);
        this.object3D.add(this._brushLayer);
    }
    get xScale() { return this._x; }
    get yScale() { return this._y; }
    get dataset() { return this._data; }
    get markType() { return this._mark; }
    x(scale) { this._x = scale; return this; }
    y(scale) { this._y = scale; return this; }
    mark(type) { this._mark = type; return this; }
    data(arr) { this._data = arr; return this; }
    render() {
        this._marks.clear();
        this._axes.clear();
        if (!this._x || !this._y || this._data.length === 0)
            return this;
        const xa = this._opts.xAccessor ?? ((d) => d.t);
        const ya = this._opts.yAccessor ?? ((d) => d.v);
        const points = this._data.map(d => new THREE.Vector3(this._x(xa(d)), this._y(ya(d)), 0));
        if (this._mark === 'line') {
            this._marks.add(buildLineMark(points, this._opts.markOptions));
        }
        else if (this._mark === 'bar') {
            const baseline = this._opts.baseline ?? this._y(this._y.domain()[0]);
            this._marks.add(buildBarMark(points, baseline, this._opts.markOptions));
        }
        else if (this._mark === 'scatter') {
            this._marks.add(buildScatterMark(points, this._opts.markOptions));
        }
        else if (this._mark === 'arc') {
            this._marks.add(buildArcMark(this._opts.markOptions));
        }
        if (this._opts.showAxes !== false && this._mark !== 'arc') {
            this._axes.add(buildAxes(this._x, this._y));
        }
        const xDomain = this._x.domain();
        const yDomain = this._y.domain();
        const xPx0 = this._x(xDomain[0]);
        const xPx1 = this._x(xDomain[1]);
        const yPx0 = this._y(yDomain[0]);
        const yPx1 = this._y(yDomain[1]);
        this._xRange = [Math.min(xPx0, xPx1), Math.max(xPx0, xPx1)];
        const w = Math.abs(xPx1 - xPx0);
        const h = Math.abs(yPx1 - yPx0);
        if (this._brushPlane)
            this.object3D.remove(this._brushPlane);
        if (this._mark !== 'arc' && w > 0 && h > 0) {
            const plane = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ visible: false, side: THREE.DoubleSide }));
            plane.position.set((xPx0 + xPx1) / 2, (yPx0 + yPx1) / 2, 0);
            plane.userData.isBrushPlane = true;
            plane.userData.chart = this;
            this._brushPlane = plane;
            this.object3D.add(plane);
        }
        return this;
    }
    /** Convert a point in this chart's local space to a 0..1 x-fraction. */
    xFractionAt(localX) {
        const [lo, hi] = this._xRange;
        if (hi === lo)
            return 0;
        return Math.max(0, Math.min(1, (localX - lo) / (hi - lo)));
    }
    brush(xFrac0, xFrac1) {
        this.clearBrush();
        if (!this._x || !this._y || this._data.length === 0) {
            return { indices: [], count: 0, xRange: [0, 0] };
        }
        const xa = this._opts.xAccessor ?? ((d) => d.t);
        const xDomain = this._x.domain();
        const yDomain = this._y.domain();
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
        const edges = new THREE.LineSegments(new THREE.EdgesGeometry(rectGeo), new THREE.LineBasicMaterial({ color: 0x66ffff, transparent: true, opacity: 0.9, depthTest: false }));
        edges.position.copy(rect.position);
        edges.renderOrder = 991;
        this._brushLayer.add(edges);
        const indices = [];
        for (let i = 0; i < this._data.length; i++) {
            const xVal = xa(this._data[i]);
            const xPx = this._x(xVal);
            if (xPx >= xLo && xPx <= xHi)
                indices.push(i);
        }
        return { indices, count: indices.length, xRange: [xLo, xHi] };
    }
    clearBrush() {
        while (this._brushLayer.children.length) {
            const c = this._brushLayer.children[0];
            const m = c;
            if (m.geometry)
                m.geometry.dispose();
            const mat = m.material;
            if (mat)
                mat.dispose();
            this._brushLayer.remove(c);
        }
    }
    /**
     * Update with new data and smoothly tween mark positions.
     * For line marks: rebuilds geometry with crossfade.
     * For bar marks: tweens instance heights.
     * For scatter: rebuilds (no tween — point count may change).
     */
    updateData(newData) {
        if (!this._x || !this._y || newData.length === 0)
            return;
        const xa = this._opts.xAccessor ?? ((d) => d.t);
        const ya = this._opts.yAccessor ?? ((d) => d.v);
        // Recompute scale domains from new data
        const xVals = newData.map(d => xa(d));
        const yVals = newData.map(d => ya(d));
        const xMin = Math.min(...xVals);
        const xMax = Math.max(...xVals);
        const yMin = Math.min(...yVals);
        const yMax = Math.max(...yVals);
        // Update scale domains if the scale has a domain setter
        const xScale = this._x;
        const yScale = this._y;
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
        const newPoints = newData.map(d => new THREE.Vector3(this._x(xa(d)), this._y(ya(d)), 0));
        this._data = newData;
        if (this._mark === 'line') {
            // For line: rebuild the tube geometry within the existing marks group
            const lineGroup = this._marks.children[0];
            if (lineGroup) {
                updateLineMark(lineGroup, newPoints, this._opts.markOptions);
            }
            else {
                this._marks.clear();
                this._marks.add(buildLineMark(newPoints, this._opts.markOptions));
            }
        }
        else if (this._mark === 'bar') {
            const barGroup = this._marks.children[0];
            const baseline = this._opts.baseline ?? this._y(this._y.domain()[0]);
            if (barGroup) {
                updateBarMark(barGroup, newPoints, baseline, this._opts.markOptions);
            }
            else {
                this._marks.clear();
                this._marks.add(buildBarMark(newPoints, baseline, this._opts.markOptions));
            }
        }
        else {
            // For scatter/arc: full rebuild
            this._marks.clear();
            if (this._mark === 'scatter') {
                this._marks.add(buildScatterMark(newPoints, this._opts.markOptions));
            }
        }
        // Rebuild axes and brush plane
        this._axes.clear();
        if (this._opts.showAxes !== false && this._mark !== 'arc') {
            this._axes.add(buildAxes(this._x, this._y));
        }
        const xDomain = this._x.domain();
        const yDomain = this._y.domain();
        const xPx0 = this._x(xDomain[0]);
        const xPx1 = this._x(xDomain[1]);
        const yPx0 = this._y(yDomain[0]);
        const yPx1 = this._y(yDomain[1]);
        this._xRange = [Math.min(xPx0, xPx1), Math.max(xPx0, xPx1)];
        const w = Math.abs(xPx1 - xPx0);
        const h = Math.abs(yPx1 - yPx0);
        if (this._brushPlane)
            this.object3D.remove(this._brushPlane);
        if (this._mark !== 'arc' && w > 0 && h > 0) {
            const plane = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ visible: false, side: THREE.DoubleSide }));
            plane.position.set((xPx0 + xPx1) / 2, (yPx0 + yPx1) / 2, 0);
            plane.userData.isBrushPlane = true;
            plane.userData.chart = this;
            this._brushPlane = plane;
            this.object3D.add(plane);
        }
    }
    dispose() {
        this.object3D.traverse(o => {
            const m = o;
            if (m.geometry)
                m.geometry.dispose();
            const mat = m.material;
            if (Array.isArray(mat))
                mat.forEach(x => x.dispose());
            else if (mat)
                mat.dispose();
        });
    }
}
function buildAxes(xScale, yScale) {
    const g = new THREE.Group();
    g.name = 'axis-frame';
    const xDomain = xScale.domain();
    const yDomain = yScale.domain();
    const x0 = xScale(xDomain[0]);
    const x1 = xScale(xDomain[1]);
    const y0 = yScale(yDomain[0]);
    const y1 = yScale(yDomain[1]);
    const axisMat = new THREE.LineBasicMaterial({ color: 0x9a8a70, transparent: true, opacity: 0.7 });
    const xAxis = new THREE.Line(new THREE.BufferGeometry().setFromPoints([
        new THREE.Vector3(x0, y0, 0),
        new THREE.Vector3(x1, y0, 0),
    ]), axisMat);
    const yAxis = new THREE.Line(new THREE.BufferGeometry().setFromPoints([
        new THREE.Vector3(x0, y0, 0),
        new THREE.Vector3(x0, y1, 0),
    ]), axisMat);
    g.add(xAxis);
    g.add(yAxis);
    return g;
}
