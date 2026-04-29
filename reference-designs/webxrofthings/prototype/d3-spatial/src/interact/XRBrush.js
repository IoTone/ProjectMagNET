export class XRBrush {
    events;
    brushing = [false, false];
    selected = [new Set(), new Set()];
    constructor(events) {
        this.events = events;
    }
    startBrush(handIndex) {
        this.brushing[handIndex] = true;
        this.selected[handIndex] = new Set();
        this.events.onBrushStart?.(handIndex);
    }
    /** Call each frame during active brush with whatever node the ray is hitting */
    addToSelection(handIndex, nodeIndex) {
        if (!this.brushing[handIndex])
            return;
        if (!this.selected[handIndex].has(nodeIndex)) {
            this.selected[handIndex].add(nodeIndex);
            this.events.onBrushAdd?.(handIndex, nodeIndex);
        }
    }
    endBrush(handIndex) {
        if (!this.brushing[handIndex])
            return;
        this.brushing[handIndex] = false;
        const indices = [...this.selected[handIndex]];
        this.events.onBrushEnd?.(handIndex, indices);
        this.selected[handIndex].clear();
    }
    isBrushing(handIndex) {
        return this.brushing[handIndex];
    }
    getSelected(handIndex) {
        return [...(this.selected[handIndex] ?? [])];
    }
}
