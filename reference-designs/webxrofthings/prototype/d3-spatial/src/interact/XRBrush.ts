export interface XRBrushEvents {
  onBrushStart?: (handIndex: number) => void;
  onBrushAdd?: (handIndex: number, nodeIndex: number) => void;
  onBrushEnd?: (handIndex: number, selectedIndices: number[]) => void;
}

export class XRBrush {
  private brushing: boolean[] = [false, false];
  private selected: Set<number>[] = [new Set(), new Set()];

  constructor(private events: XRBrushEvents) {}

  startBrush(handIndex: number) {
    this.brushing[handIndex] = true;
    this.selected[handIndex] = new Set();
    this.events.onBrushStart?.(handIndex);
  }

  /** Call each frame during active brush with whatever node the ray is hitting */
  addToSelection(handIndex: number, nodeIndex: number) {
    if (!this.brushing[handIndex]) return;
    if (!this.selected[handIndex]!.has(nodeIndex)) {
      this.selected[handIndex]!.add(nodeIndex);
      this.events.onBrushAdd?.(handIndex, nodeIndex);
    }
  }

  endBrush(handIndex: number) {
    if (!this.brushing[handIndex]) return;
    this.brushing[handIndex] = false;
    const indices = [...this.selected[handIndex]!];
    this.events.onBrushEnd?.(handIndex, indices);
    this.selected[handIndex]!.clear();
  }

  isBrushing(handIndex: number): boolean {
    return this.brushing[handIndex]!;
  }

  getSelected(handIndex: number): number[] {
    return [...(this.selected[handIndex] ?? [])];
  }
}
