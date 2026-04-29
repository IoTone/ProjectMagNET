import * as THREE from 'three';
export class DragBrush {
    camera;
    canvas;
    charts;
    events;
    raycaster = new THREE.Raycaster();
    ndc = new THREE.Vector2();
    activeChart = null;
    startFrac = 0;
    lastResult = null;
    dragging = false;
    constructor(camera, canvas, charts, events = {}) {
        this.camera = camera;
        this.canvas = canvas;
        this.charts = charts;
        this.events = events;
        canvas.addEventListener('pointerdown', this.onDown);
        canvas.addEventListener('pointermove', this.onMove);
        window.addEventListener('pointerup', this.onUp);
    }
    setNdcFromEvent(e) {
        const r = this.canvas.getBoundingClientRect();
        this.ndc.x = ((e.clientX - r.left) / r.width) * 2 - 1;
        this.ndc.y = -((e.clientY - r.top) / r.height) * 2 + 1;
    }
    hitChart() {
        this.raycaster.setFromCamera(this.ndc, this.camera);
        const planes = this.charts()
            .map(c => c._brushPlane)
            .filter((p) => !!p);
        const hits = this.raycaster.intersectObjects(planes, false);
        if (!hits.length)
            return null;
        const hit = hits[0];
        const chart = hit.object.userData.chart;
        const local = chart.object3D.worldToLocal(hit.point.clone());
        return { chart, localX: local.x };
    }
    onDown = (e) => {
        if (e.button !== 0)
            return;
        this.setNdcFromEvent(e);
        const hit = this.hitChart();
        if (!hit)
            return;
        this.dragging = true;
        this.activeChart = hit.chart;
        this.startFrac = hit.chart.xFractionAt(hit.localX);
        this.events.onStart?.(hit.chart);
        this.updateBrush(this.startFrac);
        this.canvas.setPointerCapture(e.pointerId);
    };
    onMove = (e) => {
        if (!this.dragging || !this.activeChart)
            return;
        this.setNdcFromEvent(e);
        this.raycaster.setFromCamera(this.ndc, this.camera);
        const plane = this.activeChart._brushPlane;
        if (!plane)
            return;
        const hits = this.raycaster.intersectObject(plane, false);
        if (!hits.length)
            return;
        const local = this.activeChart.object3D.worldToLocal(hits[0].point.clone());
        this.updateBrush(this.activeChart.xFractionAt(local.x));
    };
    updateBrush(currentFrac) {
        if (!this.activeChart)
            return;
        const a = Math.min(this.startFrac, currentFrac);
        const b = Math.max(this.startFrac, currentFrac);
        const res = this.activeChart.brush(a, b);
        this.lastResult = res;
        this.events.onUpdate?.(this.activeChart, res);
    }
    onUp = () => {
        if (!this.dragging || !this.activeChart)
            return;
        this.dragging = false;
        if (this.lastResult)
            this.events.onEnd?.(this.activeChart, this.lastResult);
        this.activeChart = null;
        this.lastResult = null;
    };
}
