export class FingertipGrab {
    getHandState;
    findNearestNode;
    events;
    grabState = [null, null];
    proximityState = [null, null];
    GRAB_RADIUS = 0.025;
    PINCH_THRESHOLD = 0.02;
    constructor(getHandState, findNearestNode, events) {
        this.getHandState = getHandState;
        this.findNearestNode = findNearestNode;
        this.events = events;
    }
    update() {
        for (let h = 0; h < 2; h++) {
            const state = this.getHandState(h);
            if (!state || !state.fingertipPos) {
                this.tryRelease(h);
                this.clearProximity(h);
                continue;
            }
            const isPinching = state.pinchDistance < this.PINCH_THRESHOLD;
            if (this.grabState[h] !== null) {
                // Currently grabbed
                if (isPinching) {
                    this.events.onMove?.(h, state.fingertipPos);
                }
                else {
                    this.tryRelease(h);
                }
            }
            else if (isPinching) {
                // Not grabbed, but pinching — try to grab a nearby node
                const nearest = this.findNearestNode(state.fingertipPos);
                if (nearest && nearest.distance < this.GRAB_RADIUS) {
                    this.grabState[h] = nearest.index;
                    this.clearProximity(h);
                    this.events.onGrab?.(h, nearest.index, state.fingertipPos);
                }
            }
            else {
                // Not pinching — check proximity for visual feedback
                const nearest = this.findNearestNode(state.fingertipPos);
                if (nearest && nearest.distance < this.GRAB_RADIUS) {
                    if (this.proximityState[h] !== nearest.index) {
                        this.clearProximity(h);
                        this.proximityState[h] = nearest.index;
                    }
                    this.events.onProximity?.(h, nearest.index, nearest.distance);
                }
                else {
                    this.clearProximity(h);
                }
            }
        }
    }
    tryRelease(h) {
        if (this.grabState[h] !== null) {
            this.events.onRelease?.(h);
            this.grabState[h] = null;
        }
    }
    clearProximity(h) {
        if (this.proximityState[h] !== null) {
            this.events.onProximityEnd?.(h);
            this.proximityState[h] = null;
        }
    }
    isGrabbing(handIndex) {
        return this.grabState[handIndex] !== null;
    }
    getGrabbedNode(handIndex) {
        return this.grabState[handIndex] ?? null;
    }
}
