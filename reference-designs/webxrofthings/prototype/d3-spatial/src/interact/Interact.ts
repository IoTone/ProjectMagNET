import * as THREE from 'three';

export interface HoverContext {
  instanceId?: number;
  worldPoint?: THREE.Vector3;
  distance?: number;
  hitObject?: THREE.Object3D;
  handIndex?: number;
}

export interface Hoverable {
  id: string;
  object: THREE.Object3D;
  supportsInstances?: boolean;
  onHoverIn: (ctx?: HoverContext) => void;
  onHoverOut: (ctx?: HoverContext) => void;
  onHoverInstance?: (instanceId: number | null, handIndex?: number) => void;
  onSelect?: (ctx?: HoverContext) => void;
  onDragStart?: (ctx: HoverContext) => boolean | void;
  onDragMove?: (worldPoint: THREE.Vector3, handIndex?: number) => void;
  onDragEnd?: (ctx?: HoverContext) => void;
}

interface HandState {
  hoveredId: string | null;
  hoveredInstance: number | null;
  draggingId: string | null;
  dragDistance: number;
  pressLockedHoverId: string | null;
  pressLockedInstance: number | null;
  lastHitDistance: number;
  lastHitObject?: THREE.Object3D;
  lastWorldPoint?: THREE.Vector3;
  pendingClearAt: number;
  dragLastRayOrigin: THREE.Vector3;
  dragLastRayDirection: THREE.Vector3;
}

const HAND_MOUSE = 2;

function makeHandState(): HandState {
  return {
    hoveredId: null,
    hoveredInstance: null,
    draggingId: null,
    dragDistance: 0,
    pressLockedHoverId: null,
    pressLockedInstance: null,
    lastHitDistance: 0,
    lastHitObject: undefined,
    lastWorldPoint: undefined,
    pendingClearAt: 0,
    dragLastRayOrigin: new THREE.Vector3(),
    dragLastRayDirection: new THREE.Vector3(),
  };
}

export class Interact {
  private items = new Map<string, Hoverable>();
  private handStates: HandState[] = [makeHandState(), makeHandState(), makeHandState()]; // 0, 1 = XR hands; 2 = mouse
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
  private exitDebounceMs = 150;

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
      this.setHoverForHand(HAND_MOUSE, null);
    });
  }

  add(h: Hoverable) { this.items.set(h.id, h); }
  remove(id: string) { this.items.delete(id); }

  setXrControllers(controllers: THREE.Group[]) { this.xrControllers = controllers; }

  /** Legacy: hover by id (uses mouse hand slot). */
  hoverById(id: string | null) {
    this.setHoverForHand(HAND_MOUSE, id);
  }

  /** Returns the most-recently-hovered id from any hand. */
  getHoveredId(): string | null {
    for (const hs of this.handStates) {
      if (hs.hoveredId !== null) return hs.hoveredId;
    }
    return null;
  }

  /** Returns the hovered id for a specific hand slot. */
  getHoveredIdForHand(handIndex: number): string | null {
    return this.handStates[handIndex]?.hoveredId ?? null;
  }

  /** Returns the hovered instance id for a specific hand slot. */
  getHoveredInstanceForHand(handIndex: number): number | null {
    return this.handStates[handIndex]?.hoveredInstance ?? null;
  }

  /** Legacy: triggers select on whatever the mouse hand is hovering. */
  triggerSelectOnHovered() {
    this.triggerSelectForHand(HAND_MOUSE);
  }

  triggerSelectForHand(handIndex: number) {
    const hs = this.handStates[handIndex];
    if (!hs) return false;
    const id = hs.pressLockedHoverId ?? hs.hoveredId;
    const instance = hs.pressLockedHoverId !== null ? hs.pressLockedInstance : hs.hoveredInstance;
    if (!id) return false;
    const h = this.items.get(id);
    if (h?.onSelect) {
      h.onSelect({ instanceId: instance ?? undefined, hitObject: hs.lastHitObject, worldPoint: hs.lastWorldPoint, handIndex });
      return true;
    }
    return false;
  }

  /** Legacy: press-lock for mouse hand. */
  setPressLocked(locked: boolean) {
    this.setPressLockedForHand(HAND_MOUSE, locked);
  }

  setPressLockedForHand(handIndex: number, locked: boolean) {
    const hs = this.handStates[handIndex];
    if (!hs) return;
    if (locked) {
      hs.pressLockedHoverId = hs.hoveredId;
      hs.pressLockedInstance = hs.hoveredInstance;
      hs.pendingClearAt = 0;
    } else {
      hs.pressLockedHoverId = null;
      hs.pressLockedInstance = null;
    }
  }

  /** Legacy: true if any hand is dragging. */
  isDragging(): boolean {
    return this.handStates.some(hs => hs.draggingId !== null);
  }

  isDraggingForHand(handIndex: number): boolean {
    return this.handStates[handIndex]?.draggingId !== null;
  }

  /** Legacy: begin drag for mouse hand. */
  beginDrag(): boolean {
    return this.beginDragForHand(HAND_MOUSE);
  }

  beginDragForHand(handIndex: number): boolean {
    const hs = this.handStates[handIndex];
    if (!hs || !hs.hoveredId || hs.draggingId) return false;
    const h = this.items.get(hs.hoveredId);
    if (!h?.onDragStart) return false;
    const ctx: HoverContext = { instanceId: hs.hoveredInstance ?? undefined, handIndex };
    const ok = h.onDragStart(ctx);
    if (ok === false) return false;
    hs.draggingId = hs.hoveredId;
    hs.dragDistance = hs.lastHitDistance > 0 ? hs.lastHitDistance : 0.5;
    // Save the current ray for this hand
    this.setupRayForHand(handIndex);
    hs.dragLastRayOrigin.copy(this.raycaster.ray.origin);
    hs.dragLastRayDirection.copy(this.raycaster.ray.direction);
    return true;
  }

  /** Legacy: end drag for mouse hand. */
  endDrag() {
    this.endDragForHand(HAND_MOUSE);
  }

  endDragForHand(handIndex: number) {
    const hs = this.handStates[handIndex];
    if (!hs || !hs.draggingId) return;
    const h = this.items.get(hs.draggingId);
    h?.onDragEnd?.({ handIndex });
    hs.draggingId = null;
  }

  update() {
    if (this.items.size === 0) return;

    const isXr = this.renderer?.xr.isPresenting;

    if (isXr && this.xrControllers.length) {
      // Process each XR controller independently
      for (let i = 0; i < this.xrControllers.length; i++) {
        const hs = this.handStates[i];
        if (!hs) continue;
        const c = this.xrControllers[i];
        if (!c || !c.visible) {
          // Controller not visible — if dragging, keep drag; otherwise skip
          if (!hs.draggingId) continue;
        }

        if (hs.draggingId) {
          this.updateDragRayForHand(i);
          this.tickDragForHand(i);
          continue;
        }
        if (hs.pressLockedHoverId !== null) continue;

        const pick = this.raycastFromController(i);
        this.applyPickForHand(i, pick);
      }
    } else if (this.mouseEnabled) {
      // Desktop mouse path — uses hand slot HAND_MOUSE
      const hs = this.handStates[HAND_MOUSE]!;
      if (hs.draggingId) {
        this.updateDragRayForHand(HAND_MOUSE);
        this.tickDragForHand(HAND_MOUSE);
        return;
      }
      if (hs.pressLockedHoverId !== null) return;

      this.raycaster.setFromCamera(this.mouseNdc, this.camera);
      const pick = this.pickFromRaycaster();
      this.applyPickForHand(HAND_MOUSE, pick);
    }
  }

  /** Set the exit-debounce window (ms). 0 disables debounce. Default 150. */
  setExitDebounceMs(ms: number) { this.exitDebounceMs = Math.max(0, ms); }

  private setupRayForHand(handIndex: number) {
    if (handIndex === HAND_MOUSE) {
      this.raycaster.setFromCamera(this.mouseNdc, this.camera);
      return;
    }
    const c = this.xrControllers[handIndex];
    if (c && c.visible) {
      this.tempMatrix.identity().extractRotation(c.matrixWorld);
      this.raycaster.ray.origin.setFromMatrixPosition(c.matrixWorld);
      this.raycaster.ray.direction.set(0, 0, -1).applyMatrix4(this.tempMatrix);
    }
  }

  private updateDragRayForHand(handIndex: number) {
    if (handIndex === HAND_MOUSE) {
      if (this.mouseEnabled) this.raycaster.setFromCamera(this.mouseNdc, this.camera);
      return;
    }
    const c = this.xrControllers[handIndex];
    if (c && c.visible) {
      this.tempMatrix.identity().extractRotation(c.matrixWorld);
      this.raycaster.ray.origin.setFromMatrixPosition(c.matrixWorld);
      this.raycaster.ray.direction.set(0, 0, -1).applyMatrix4(this.tempMatrix);
    }
  }

  private tickDragForHand(handIndex: number) {
    const hs = this.handStates[handIndex];
    if (!hs || !hs.draggingId) return;
    const h = this.items.get(hs.draggingId);
    if (!h) return;
    const target = new THREE.Vector3()
      .copy(this.raycaster.ray.direction)
      .multiplyScalar(hs.dragDistance)
      .add(this.raycaster.ray.origin);
    h.onDragMove?.(target, handIndex);
  }

  private applyPickForHand(handIndex: number, pick: { id: string; instanceId?: number; distance: number; hitObject?: THREE.Object3D; worldPoint?: THREE.Vector3 } | null) {
    const hs = this.handStates[handIndex];
    if (!hs) return;

    if (pick) {
      hs.lastHitDistance = pick.distance;
      hs.lastHitObject = pick.hitObject;
      hs.lastWorldPoint = pick.worldPoint;
      hs.pendingClearAt = 0;
      this.setHoverForHand(handIndex, pick.id, pick.instanceId ?? null);
      return;
    }
    hs.lastHitDistance = 0;
    if (hs.hoveredId === null) return;
    if (this.exitDebounceMs <= 0) { this.setHoverForHand(handIndex, null, null); return; }
    const now = performance.now();
    if (hs.pendingClearAt === 0) {
      hs.pendingClearAt = now + this.exitDebounceMs;
    } else if (now >= hs.pendingClearAt) {
      hs.pendingClearAt = 0;
      this.setHoverForHand(handIndex, null, null);
    }
  }

  private raycastFromController(controllerIndex: number): { id: string; instanceId?: number; distance: number; hitObject?: THREE.Object3D; worldPoint?: THREE.Vector3 } | null {
    const c = this.xrControllers[controllerIndex];
    if (!c || !c.visible) return null;
    this.tempMatrix.identity().extractRotation(c.matrixWorld);
    this.raycaster.ray.origin.setFromMatrixPosition(c.matrixWorld);
    this.raycaster.ray.direction.set(0, 0, -1).applyMatrix4(this.tempMatrix);
    return this.pickFromRaycaster();
  }

  private pickFromRaycaster(): { id: string; instanceId?: number; distance: number; hitObject?: THREE.Object3D; worldPoint?: THREE.Vector3 } | null {
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
      // Invisible subtrees must never be hit. THREE.Raycaster ignores the
      // `.visible` flag entirely — it only gates *rendering*, not
      // raycasting — so a hidden panel still intercepts rays. This is
      // exactly the join-keypad bug: the keypad is created hidden
      // (g.visible=false) but its ~38 blocks were still ray targets,
      // blocking the sign-in slots behind/around them. Walk ancestors;
      // reject the hit if any node up the chain is invisible.
      for (let p: THREE.Object3D | null = o; p; p = p.parent) {
        if (p.visible === false) return false;
      }
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

  private setHoverForHand(handIndex: number, id: string | null, instanceId: number | null = null) {
    const hs = this.handStates[handIndex];
    if (!hs) return;

    if (id === hs.hoveredId && instanceId === hs.hoveredInstance) return;
    if (id === hs.hoveredId && id) {
      this.items.get(id)?.onHoverInstance?.(instanceId, handIndex);
      hs.hoveredInstance = instanceId;
      return;
    }
    if (hs.hoveredId) {
      this.items.get(hs.hoveredId)?.onHoverInstance?.(null, handIndex);
      this.items.get(hs.hoveredId)?.onHoverOut({ handIndex });
    }
    hs.hoveredId = id;
    hs.hoveredInstance = instanceId;
    if (id) {
      const h = this.items.get(id);
      h?.onHoverIn({ instanceId: instanceId ?? undefined, hitObject: hs.lastHitObject, worldPoint: hs.lastWorldPoint, handIndex });
      if (instanceId !== null) h?.onHoverInstance?.(instanceId, handIndex);
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
