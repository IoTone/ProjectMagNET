import * as THREE from 'three';

export interface HoverContext {
  instanceId?: number;
  worldPoint?: THREE.Vector3;
  distance?: number;
  hitObject?: THREE.Object3D;
}

export interface Hoverable {
  id: string;
  object: THREE.Object3D;
  supportsInstances?: boolean;
  onHoverIn: (ctx?: HoverContext) => void;
  onHoverOut: () => void;
  onHoverInstance?: (instanceId: number | null) => void;
  onSelect?: (ctx?: HoverContext) => void;
  onDragStart?: (ctx: HoverContext) => boolean | void;
  onDragMove?: (worldPoint: THREE.Vector3) => void;
  onDragEnd?: () => void;
}

export class Interact {
  private items = new Map<string, Hoverable>();
  private hoveredId: string | null = null;
  private hoveredInstance: number | null = null;
  private draggingId: string | null = null;
  private dragDistance = 0;
  private dragLastRayOrigin = new THREE.Vector3();
  private dragLastRayDirection = new THREE.Vector3();
  private raycaster = (() => {
    const r = new THREE.Raycaster();
    r.params.Line = { threshold: 0.0005 };
    r.params.Points = { threshold: 0.005 };
    return r;
  })();
  private mouseNdc = new THREE.Vector2();
  private mouseEnabled = false;
  private xrControllers: THREE.Group[] = [];
  private tempMatrix = new THREE.Matrix4();
  private lastHitDistance = 0;
  private exitDebounceMs = 150;
  private pendingClearAt = 0;
  private pressLockedHoverId: string | null = null;
  private pressLockedInstance: number | null = null;

  constructor(
    private camera: THREE.Camera,
    private canvas: HTMLCanvasElement,
    private renderer?: THREE.WebGLRenderer,
  ) {
    canvas.addEventListener('pointermove', e => {
      this.mouseEnabled = true;
      const r = canvas.getBoundingClientRect();
      this.mouseNdc.x = ((e.clientX - r.left) / r.width) * 2 - 1;
      this.mouseNdc.y = -((e.clientY - r.top) / r.height) * 2 + 1;
    });
    canvas.addEventListener('pointerleave', () => {
      this.mouseEnabled = false;
      this.setHover(null);
    });
  }

  add(h: Hoverable) { this.items.set(h.id, h); }
  remove(id: string) { this.items.delete(id); }

  setXrControllers(controllers: THREE.Group[]) { this.xrControllers = controllers; }

  hoverById(id: string | null) {
    this.setHover(id);
  }

  getHoveredId(): string | null { return this.hoveredId; }

  triggerSelectOnHovered() {
    const id = this.pressLockedHoverId ?? this.hoveredId;
    const instance = this.pressLockedHoverId !== null ? this.pressLockedInstance : this.hoveredInstance;
    if (!id) return false;
    const h = this.items.get(id);
    if (h?.onSelect) { h.onSelect({ instanceId: instance ?? undefined, hitObject: this.lastHitObject, worldPoint: this.lastWorldPoint }); return true; }
    return false;
  }

  setPressLocked(locked: boolean) {
    if (locked) {
      this.pressLockedHoverId = this.hoveredId;
      this.pressLockedInstance = this.hoveredInstance;
      this.pendingClearAt = 0;
    } else {
      this.pressLockedHoverId = null;
      this.pressLockedInstance = null;
    }
  }

  isDragging(): boolean { return this.draggingId !== null; }

  beginDrag(): boolean {
    if (!this.hoveredId || this.draggingId) return false;
    const h = this.items.get(this.hoveredId);
    if (!h?.onDragStart) return false;
    const ctx: HoverContext = { instanceId: this.hoveredInstance ?? undefined };
    const ok = h.onDragStart(ctx);
    if (ok === false) return false;
    this.draggingId = this.hoveredId;
    this.dragDistance = this.lastHitDistance > 0 ? this.lastHitDistance : 0.5;
    this.dragLastRayOrigin.copy(this.raycaster.ray.origin);
    this.dragLastRayDirection.copy(this.raycaster.ray.direction);
    return true;
  }

  endDrag() {
    if (!this.draggingId) return;
    const h = this.items.get(this.draggingId);
    h?.onDragEnd?.();
    this.draggingId = null;
  }

  update() {
    if (this.items.size === 0) return;
    if (this.draggingId) {
      this.updateDragRay();
      this.tickDrag();
      return;
    }
    if (this.pressLockedHoverId !== null) return;

    const isXr = this.renderer?.xr.isPresenting;
    let pick: { id: string; instanceId?: number; distance: number } | null = null;
    if (isXr && this.xrControllers.length) {
      pick = this.raycastFromControllers();
    } else if (this.mouseEnabled) {
      this.raycaster.setFromCamera(this.mouseNdc, this.camera);
      pick = this.pickFromRaycaster();
    } else {
      return;
    }
    this.applyPick(pick);
  }

  /** Set the exit-debounce window (ms). 0 disables debounce. Default 150. */
  setExitDebounceMs(ms: number) { this.exitDebounceMs = Math.max(0, ms); }

  private updateDragRay() {
    const isXr = this.renderer?.xr.isPresenting;
    if (isXr && this.xrControllers.length) {
      for (const c of this.xrControllers) {
        if (!c.visible) continue;
        this.tempMatrix.identity().extractRotation(c.matrixWorld);
        this.raycaster.ray.origin.setFromMatrixPosition(c.matrixWorld);
        this.raycaster.ray.direction.set(0, 0, -1).applyMatrix4(this.tempMatrix);
        return;
      }
    }
    if (this.mouseEnabled) this.raycaster.setFromCamera(this.mouseNdc, this.camera);
  }

  private tickDrag() {
    const h = this.items.get(this.draggingId!);
    if (!h) return;
    const target = new THREE.Vector3()
      .copy(this.raycaster.ray.direction)
      .multiplyScalar(this.dragDistance)
      .add(this.raycaster.ray.origin);
    h.onDragMove?.(target);
  }

  private lastHitObject: THREE.Object3D | undefined = undefined;
  private lastWorldPoint: THREE.Vector3 | undefined = undefined;

  private applyPick(pick: { id: string; instanceId?: number; distance: number; hitObject?: THREE.Object3D; worldPoint?: THREE.Vector3 } | null) {
    if (pick) {
      this.lastHitDistance = pick.distance;
      this.lastHitObject = pick.hitObject;
      this.lastWorldPoint = pick.worldPoint;
      this.pendingClearAt = 0;
      this.setHover(pick.id, pick.instanceId ?? null);
      return;
    }
    this.lastHitDistance = 0;
    if (this.hoveredId === null) return;
    if (this.exitDebounceMs <= 0) { this.setHover(null, null); return; }
    const now = performance.now();
    if (this.pendingClearAt === 0) {
      this.pendingClearAt = now + this.exitDebounceMs;
    } else if (now >= this.pendingClearAt) {
      this.pendingClearAt = 0;
      this.setHover(null, null);
    }
  }

  private raycastFromControllers(): { id: string; instanceId?: number; distance: number } | null {
    const objects = [...this.items.values()].map(i => i.object);
    for (const c of this.xrControllers) {
      if (!c.visible) continue;
      this.tempMatrix.identity().extractRotation(c.matrixWorld);
      this.raycaster.ray.origin.setFromMatrixPosition(c.matrixWorld);
      this.raycaster.ray.direction.set(0, 0, -1).applyMatrix4(this.tempMatrix);
      const hits = this.filterHits(this.raycaster.intersectObjects(objects, true));
      if (hits.length) return this.hitToPick(hits[0]!);
    }
    return null;
  }

  private pickFromRaycaster(): { id: string; instanceId?: number; distance: number } | null {
    const objects = [...this.items.values()].map(i => i.object);
    const hits = this.filterHits(this.raycaster.intersectObjects(objects, true));
    if (!hits.length) return null;
    return this.hitToPick(hits[0]!);
  }

  private hitToPick(hit: THREE.Intersection): { id: string; instanceId?: number; distance: number; hitObject?: THREE.Object3D; worldPoint?: THREE.Vector3 } | null {
    const id = this.findRootId(hit.object);
    if (!id) return null;
    const h = this.items.get(id);
    const wantInstance = h?.supportsInstances && hit.instanceId !== undefined;
    return { id, instanceId: wantInstance ? hit.instanceId : undefined, distance: hit.distance, hitObject: hit.object, worldPoint: hit.point?.clone() };
  }

  private filterHits(hits: THREE.Intersection[]): THREE.Intersection[] {
    return hits.filter(h => {
      const o = h.object;
      if (o.userData?.isBrushPlane || o.userData?.isHoverOutline) return false;
      if ((o as any).isLine || (o as any).isLineSegments) return false;
      return true;
    });
  }

  private findRootId(obj: THREE.Object3D): string | null {
    for (const [id, h] of this.items) {
      let o: THREE.Object3D | null = obj;
      while (o) { if (o === h.object) return id; o = o.parent; }
    }
    return null;
  }

  private setHover(id: string | null, instanceId: number | null = null) {
    if (id === this.hoveredId && instanceId === this.hoveredInstance) return;
    if (id === this.hoveredId && id) {
      this.items.get(id)?.onHoverInstance?.(instanceId);
      this.hoveredInstance = instanceId;
      return;
    }
    if (this.hoveredId) {
      this.items.get(this.hoveredId)?.onHoverInstance?.(null);
      this.items.get(this.hoveredId)?.onHoverOut();
    }
    this.hoveredId = id;
    this.hoveredInstance = instanceId;
    if (id) {
      const h = this.items.get(id);
      h?.onHoverIn({ instanceId: instanceId ?? undefined, hitObject: this.lastHitObject, worldPoint: this.lastWorldPoint });
      if (instanceId !== null) h?.onHoverInstance?.(instanceId);
    }
  }
}

export function hoverFeedback(obj: THREE.Object3D) {
  const originalScale = obj.scale.clone();

  const removeAllOutlines = () => {
    const orphans = obj.children.filter(c => c.userData?.isHoverOutline);
    for (const o of orphans) {
      obj.remove(o);
      const ls = o as THREE.LineSegments;
      ls.geometry?.dispose();
      (ls.material as THREE.Material | undefined)?.dispose();
    }
  };

  return {
    on() {
      removeAllOutlines();
      obj.scale.copy(originalScale).multiplyScalar(1.05);
      obj.traverse(o => {
        const m = o as THREE.Mesh;
        const mat = m.material as THREE.MeshStandardMaterial | undefined;
        if (mat && 'emissiveIntensity' in mat) {
          (mat as any).__origEmissive = (mat as any).__origEmissive ?? mat.emissiveIntensity;
          mat.emissiveIntensity = Math.min(1.2, (mat as any).__origEmissive + 0.6);
        }
      });
      const bbox = new THREE.Box3().setFromObject(obj);
      const size = bbox.getSize(new THREE.Vector3());
      const center = bbox.getCenter(new THREE.Vector3());
      const box = new THREE.BoxGeometry(size.x * 1.08, size.y * 1.1, Math.max(size.z, 0.01) * 1.5);
      const outline = new THREE.LineSegments(
        new THREE.EdgesGeometry(box),
        new THREE.LineBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.85, depthTest: false }),
      );
      outline.userData.isHoverOutline = true;
      outline.position.copy(obj.worldToLocal(center.clone()));
      outline.renderOrder = 995;
      obj.add(outline);
    },
    off() {
      obj.scale.copy(originalScale);
      obj.traverse(o => {
        const m = o as THREE.Mesh;
        const mat = m.material as THREE.MeshStandardMaterial | undefined;
        if (mat && (mat as any).__origEmissive !== undefined) {
          mat.emissiveIntensity = (mat as any).__origEmissive;
        }
      });
      removeAllOutlines();
    },
  };
}
