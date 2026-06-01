import * as THREE from 'three';
import { Chart, BrushResult } from '../chart/Chart';

export interface DragBrushEvents {
  onStart?: (chart: Chart) => void;
  onUpdate?: (chart: Chart, res: BrushResult) => void;
  onEnd?: (chart: Chart, res: BrushResult) => void;
  onClear?: () => void;
}

export class DragBrush {
  private raycaster = new THREE.Raycaster();
  private ndc = new THREE.Vector2();
  private activeChart: Chart | null = null;
  private startFrac = 0;
  private lastResult: BrushResult | null = null;
  public dragging = false;

  constructor(
    private camera: THREE.Camera,
    private canvas: HTMLCanvasElement,
    private charts: () => Chart[],
    private events: DragBrushEvents = {},
  ) {
    canvas.addEventListener('pointerdown', this.onDown);
    canvas.addEventListener('pointermove', this.onMove);
    window.addEventListener('pointerup', this.onUp);
  }

  private setNdcFromEvent(e: PointerEvent) {
    const r = this.canvas.getBoundingClientRect();
    this.ndc.x = ((e.clientX - r.left) / r.width) * 2 - 1;
    this.ndc.y = -((e.clientY - r.top) / r.height) * 2 + 1;
  }

  private hitChart(): { chart: Chart; localX: number } | null {
    this.raycaster.setFromCamera(this.ndc, this.camera);
    const planes = this.charts()
      .map(c => (c as any)._brushPlane as THREE.Mesh | null)
      .filter((p): p is THREE.Mesh => !!p);
    const hits = this.raycaster.intersectObjects(planes, false);
    if (!hits.length) return null;
    const hit = hits[0]!;
    const chart = hit.object.userData.chart as Chart;
    const local = chart.object3D.worldToLocal(hit.point.clone());
    return { chart, localX: local.x };
  }

  private onDown = (e: PointerEvent) => {
    if (e.button !== 0) return;
    this.setNdcFromEvent(e);
    const hit = this.hitChart();
    if (!hit) return;
    this.dragging = true;
    this.activeChart = hit.chart;
    this.startFrac = hit.chart.xFractionAt(hit.localX);
    this.events.onStart?.(hit.chart);
    this.updateBrush(this.startFrac);
    this.canvas.setPointerCapture(e.pointerId);
  };

  private onMove = (e: PointerEvent) => {
    if (!this.dragging || !this.activeChart) return;
    this.setNdcFromEvent(e);
    this.raycaster.setFromCamera(this.ndc, this.camera);
    const plane = (this.activeChart as any)._brushPlane as THREE.Mesh | null;
    if (!plane) return;
    const hits = this.raycaster.intersectObject(plane, false);
    if (!hits.length) return;
    const local = this.activeChart.object3D.worldToLocal(hits[0]!.point.clone());
    this.updateBrush(this.activeChart.xFractionAt(local.x));
  };

  private updateBrush(currentFrac: number) {
    if (!this.activeChart) return;
    const a = Math.min(this.startFrac, currentFrac);
    const b = Math.max(this.startFrac, currentFrac);
    const res = this.activeChart.brush(a, b);
    this.lastResult = res;
    this.events.onUpdate?.(this.activeChart, res);
  }

  private onUp = () => {
    if (!this.dragging || !this.activeChart) return;
    this.dragging = false;
    if (this.lastResult) this.events.onEnd?.(this.activeChart, this.lastResult);
    this.activeChart = null;
    this.lastResult = null;
  };
}
