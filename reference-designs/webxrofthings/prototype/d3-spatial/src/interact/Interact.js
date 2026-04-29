import * as THREE from 'three';
const HAND_MOUSE = 2;
function makeHandState() {
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
    camera;
    canvas;
    renderer;
    items = new Map();
    handStates = [makeHandState(), makeHandState(), makeHandState()]; // 0, 1 = XR hands; 2 = mouse
    raycaster = (() => {
        const r = new THREE.Raycaster();
        r.params.Line = { threshold: 0.0005 };
        r.params.Points = { threshold: 0.005 };
        return r;
    })();
    mouseNdc = new THREE.Vector2();
    mouseEnabled = false;
    xrControllers = [];
    tempMatrix = new THREE.Matrix4();
    exitDebounceMs = 150;
    constructor(camera, canvas, renderer) {
        this.camera = camera;
        this.canvas = canvas;
        this.renderer = renderer;
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
    add(h) { this.items.set(h.id, h); }
    remove(id) { this.items.delete(id); }
    setXrControllers(controllers) { this.xrControllers = controllers; }
    /** Legacy: hover by id (uses mouse hand slot). */
    hoverById(id) {
        this.setHoverForHand(HAND_MOUSE, id);
    }
    /** Returns the most-recently-hovered id from any hand. */
    getHoveredId() {
        for (const hs of this.handStates) {
            if (hs.hoveredId !== null)
                return hs.hoveredId;
        }
        return null;
    }
    /** Returns the hovered id for a specific hand slot. */
    getHoveredIdForHand(handIndex) {
        return this.handStates[handIndex]?.hoveredId ?? null;
    }
    /** Returns the hovered instance id for a specific hand slot. */
    getHoveredInstanceForHand(handIndex) {
        return this.handStates[handIndex]?.hoveredInstance ?? null;
    }
    /** Legacy: triggers select on whatever the mouse hand is hovering. */
    triggerSelectOnHovered() {
        this.triggerSelectForHand(HAND_MOUSE);
    }
    triggerSelectForHand(handIndex) {
        const hs = this.handStates[handIndex];
        if (!hs)
            return false;
        const id = hs.pressLockedHoverId ?? hs.hoveredId;
        const instance = hs.pressLockedHoverId !== null ? hs.pressLockedInstance : hs.hoveredInstance;
        if (!id)
            return false;
        const h = this.items.get(id);
        if (h?.onSelect) {
            h.onSelect({ instanceId: instance ?? undefined, hitObject: hs.lastHitObject, worldPoint: hs.lastWorldPoint, handIndex });
            return true;
        }
        return false;
    }
    /** Legacy: press-lock for mouse hand. */
    setPressLocked(locked) {
        this.setPressLockedForHand(HAND_MOUSE, locked);
    }
    setPressLockedForHand(handIndex, locked) {
        const hs = this.handStates[handIndex];
        if (!hs)
            return;
        if (locked) {
            hs.pressLockedHoverId = hs.hoveredId;
            hs.pressLockedInstance = hs.hoveredInstance;
            hs.pendingClearAt = 0;
        }
        else {
            hs.pressLockedHoverId = null;
            hs.pressLockedInstance = null;
        }
    }
    /** Legacy: true if any hand is dragging. */
    isDragging() {
        return this.handStates.some(hs => hs.draggingId !== null);
    }
    isDraggingForHand(handIndex) {
        return this.handStates[handIndex]?.draggingId !== null;
    }
    /** Legacy: begin drag for mouse hand. */
    beginDrag() {
        return this.beginDragForHand(HAND_MOUSE);
    }
    beginDragForHand(handIndex) {
        const hs = this.handStates[handIndex];
        if (!hs || !hs.hoveredId || hs.draggingId)
            return false;
        const h = this.items.get(hs.hoveredId);
        if (!h?.onDragStart)
            return false;
        const ctx = { instanceId: hs.hoveredInstance ?? undefined, handIndex };
        const ok = h.onDragStart(ctx);
        if (ok === false)
            return false;
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
    endDragForHand(handIndex) {
        const hs = this.handStates[handIndex];
        if (!hs || !hs.draggingId)
            return;
        const h = this.items.get(hs.draggingId);
        h?.onDragEnd?.({ handIndex });
        hs.draggingId = null;
    }
    update() {
        if (this.items.size === 0)
            return;
        const isXr = this.renderer?.xr.isPresenting;
        if (isXr && this.xrControllers.length) {
            // Process each XR controller independently
            for (let i = 0; i < this.xrControllers.length; i++) {
                const hs = this.handStates[i];
                if (!hs)
                    continue;
                const c = this.xrControllers[i];
                if (!c || !c.visible) {
                    // Controller not visible — if dragging, keep drag; otherwise skip
                    if (!hs.draggingId)
                        continue;
                }
                if (hs.draggingId) {
                    this.updateDragRayForHand(i);
                    this.tickDragForHand(i);
                    continue;
                }
                if (hs.pressLockedHoverId !== null)
                    continue;
                const pick = this.raycastFromController(i);
                this.applyPickForHand(i, pick);
            }
        }
        else if (this.mouseEnabled) {
            // Desktop mouse path — uses hand slot HAND_MOUSE
            const hs = this.handStates[HAND_MOUSE];
            if (hs.draggingId) {
                this.updateDragRayForHand(HAND_MOUSE);
                this.tickDragForHand(HAND_MOUSE);
                return;
            }
            if (hs.pressLockedHoverId !== null)
                return;
            this.raycaster.setFromCamera(this.mouseNdc, this.camera);
            const pick = this.pickFromRaycaster();
            this.applyPickForHand(HAND_MOUSE, pick);
        }
    }
    /** Set the exit-debounce window (ms). 0 disables debounce. Default 150. */
    setExitDebounceMs(ms) { this.exitDebounceMs = Math.max(0, ms); }
    setupRayForHand(handIndex) {
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
    updateDragRayForHand(handIndex) {
        if (handIndex === HAND_MOUSE) {
            if (this.mouseEnabled)
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
    tickDragForHand(handIndex) {
        const hs = this.handStates[handIndex];
        if (!hs || !hs.draggingId)
            return;
        const h = this.items.get(hs.draggingId);
        if (!h)
            return;
        const target = new THREE.Vector3()
            .copy(this.raycaster.ray.direction)
            .multiplyScalar(hs.dragDistance)
            .add(this.raycaster.ray.origin);
        h.onDragMove?.(target, handIndex);
    }
    applyPickForHand(handIndex, pick) {
        const hs = this.handStates[handIndex];
        if (!hs)
            return;
        if (pick) {
            hs.lastHitDistance = pick.distance;
            hs.lastHitObject = pick.hitObject;
            hs.lastWorldPoint = pick.worldPoint;
            hs.pendingClearAt = 0;
            this.setHoverForHand(handIndex, pick.id, pick.instanceId ?? null);
            return;
        }
        hs.lastHitDistance = 0;
        if (hs.hoveredId === null)
            return;
        if (this.exitDebounceMs <= 0) {
            this.setHoverForHand(handIndex, null, null);
            return;
        }
        const now = performance.now();
        if (hs.pendingClearAt === 0) {
            hs.pendingClearAt = now + this.exitDebounceMs;
        }
        else if (now >= hs.pendingClearAt) {
            hs.pendingClearAt = 0;
            this.setHoverForHand(handIndex, null, null);
        }
    }
    raycastFromController(controllerIndex) {
        const c = this.xrControllers[controllerIndex];
        if (!c || !c.visible)
            return null;
        this.tempMatrix.identity().extractRotation(c.matrixWorld);
        this.raycaster.ray.origin.setFromMatrixPosition(c.matrixWorld);
        this.raycaster.ray.direction.set(0, 0, -1).applyMatrix4(this.tempMatrix);
        return this.pickFromRaycaster();
    }
    pickFromRaycaster() {
        const objects = [...this.items.values()].map(i => i.object);
        const hits = this.filterHits(this.raycaster.intersectObjects(objects, true));
        if (!hits.length)
            return null;
        return this.hitToPick(hits[0]);
    }
    hitToPick(hit) {
        const id = this.findRootId(hit.object);
        if (!id)
            return null;
        const h = this.items.get(id);
        const wantInstance = h?.supportsInstances && hit.instanceId !== undefined;
        return { id, instanceId: wantInstance ? hit.instanceId : undefined, distance: hit.distance, hitObject: hit.object, worldPoint: hit.point?.clone() };
    }
    filterHits(hits) {
        return hits.filter(h => {
            const o = h.object;
            if (o.userData?.isBrushPlane || o.userData?.isHoverOutline)
                return false;
            if (o.isLine || o.isLineSegments)
                return false;
            return true;
        });
    }
    findRootId(obj) {
        for (const [id, h] of this.items) {
            let o = obj;
            while (o) {
                if (o === h.object)
                    return id;
                o = o.parent;
            }
        }
        return null;
    }
    setHoverForHand(handIndex, id, instanceId = null) {
        const hs = this.handStates[handIndex];
        if (!hs)
            return;
        if (id === hs.hoveredId && instanceId === hs.hoveredInstance)
            return;
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
            if (instanceId !== null)
                h?.onHoverInstance?.(instanceId, handIndex);
        }
    }
}
export function hoverFeedback(obj) {
    const originalScale = obj.scale.clone();
    const removeAllOutlines = () => {
        const orphans = obj.children.filter(c => c.userData?.isHoverOutline);
        for (const o of orphans) {
            obj.remove(o);
            const ls = o;
            ls.geometry?.dispose();
            ls.material?.dispose();
        }
    };
    return {
        on() {
            removeAllOutlines();
            obj.scale.copy(originalScale).multiplyScalar(1.05);
            obj.traverse(o => {
                const m = o;
                const mat = m.material;
                if (mat && 'emissiveIntensity' in mat) {
                    mat.__origEmissive = mat.__origEmissive ?? mat.emissiveIntensity;
                    mat.emissiveIntensity = Math.min(1.2, mat.__origEmissive + 0.6);
                }
            });
            const bbox = new THREE.Box3().setFromObject(obj);
            const size = bbox.getSize(new THREE.Vector3());
            const center = bbox.getCenter(new THREE.Vector3());
            const box = new THREE.BoxGeometry(size.x * 1.08, size.y * 1.1, Math.max(size.z, 0.01) * 1.5);
            const outline = new THREE.LineSegments(new THREE.EdgesGeometry(box), new THREE.LineBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.85, depthTest: false }));
            outline.userData.isHoverOutline = true;
            outline.position.copy(obj.worldToLocal(center.clone()));
            outline.renderOrder = 995;
            obj.add(outline);
        },
        off() {
            obj.scale.copy(originalScale);
            obj.traverse(o => {
                const m = o;
                const mat = m.material;
                if (mat && mat.__origEmissive !== undefined) {
                    mat.emissiveIntensity = mat.__origEmissive;
                }
            });
            removeAllOutlines();
        },
    };
}
