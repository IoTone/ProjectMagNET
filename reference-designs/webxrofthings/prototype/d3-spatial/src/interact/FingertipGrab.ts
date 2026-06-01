import * as THREE from 'three';

export interface HandJointState {
  fingertipPos: THREE.Vector3;
  pinchDistance: number;
}

export interface FingertipGrabEvents {
  onGrab?: (handIndex: number, nearestNodeIndex: number, worldPos: THREE.Vector3) => void;
  onMove?: (handIndex: number, worldPos: THREE.Vector3) => void;
  onRelease?: (handIndex: number) => void;
  onProximity?: (handIndex: number, nearestNodeIndex: number, distance: number) => void;
  onProximityEnd?: (handIndex: number) => void;
}

export class FingertipGrab {
  private grabState: (number | null)[] = [null, null];
  private proximityState: (number | null)[] = [null, null];
  private readonly GRAB_RADIUS = 0.025;
  private readonly PINCH_THRESHOLD = 0.02;

  constructor(
    private getHandState: (i: number) => HandJointState | null,
    private findNearestNode: (worldPos: THREE.Vector3) => { index: number; distance: number } | null,
    private events: FingertipGrabEvents,
  ) {}

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
        } else {
          this.tryRelease(h);
        }
      } else if (isPinching) {
        // Not grabbed, but pinching — try to grab a nearby node
        const nearest = this.findNearestNode(state.fingertipPos);
        if (nearest && nearest.distance < this.GRAB_RADIUS) {
          this.grabState[h] = nearest.index;
          this.clearProximity(h);
          this.events.onGrab?.(h, nearest.index, state.fingertipPos);
        }
      } else {
        // Not pinching — check proximity for visual feedback
        const nearest = this.findNearestNode(state.fingertipPos);
        if (nearest && nearest.distance < this.GRAB_RADIUS) {
          if (this.proximityState[h] !== nearest.index) {
            this.clearProximity(h);
            this.proximityState[h] = nearest.index;
          }
          this.events.onProximity?.(h, nearest.index, nearest.distance);
        } else {
          this.clearProximity(h);
        }
      }
    }
  }

  private tryRelease(h: number) {
    if (this.grabState[h] !== null) {
      this.events.onRelease?.(h);
      this.grabState[h] = null;
    }
  }

  private clearProximity(h: number) {
    if (this.proximityState[h] !== null) {
      this.events.onProximityEnd?.(h);
      this.proximityState[h] = null;
    }
  }

  isGrabbing(handIndex: number): boolean {
    return this.grabState[handIndex] !== null;
  }

  getGrabbedNode(handIndex: number): number | null {
    return this.grabState[handIndex] ?? null;
  }
}
