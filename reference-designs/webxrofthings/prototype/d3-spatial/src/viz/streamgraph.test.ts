import { describe, it, expect } from 'vitest';
import * as THREE from 'three';
import { buildStreamgraph } from './streamgraph';
import { sampleStreamgraph } from '../demo/sampleHierarchy';

describe('buildStreamgraph', () => {
  it('returns a group with one mesh per category', () => {
    const series = sampleStreamgraph(4, 50);
    const viz = buildStreamgraph(series);
    const meshes = viz.group.children.filter(c => (c as THREE.Mesh).isMesh);
    expect(meshes).toHaveLength(4);
  });

  it('initial mesh positions are non-zero (initial frame written eagerly)', () => {
    const series = sampleStreamgraph(3, 50);
    const viz = buildStreamgraph(series);
    const mesh = viz.group.children[0] as THREE.Mesh;
    const pos = mesh.geometry.getAttribute('position') as THREE.BufferAttribute;
    let nonZero = 0;
    for (let i = 0; i < pos.array.length; i++) {
      if ((pos.array as Float32Array)[i] !== 0) nonZero++;
    }
    // We have x and y coordinates; some should be non-zero.
    expect(nonZero).toBeGreaterThan(0);
  });

  it('tick() bumps the position attribute version (signals GPU re-upload)', () => {
    const series = sampleStreamgraph(3, 50);
    const viz = buildStreamgraph(series);
    const mesh = viz.group.children[0] as THREE.Mesh;
    const pos = mesh.geometry.getAttribute('position') as THREE.BufferAttribute;
    const before = pos.version;
    viz.tick(1.5);
    expect(pos.version).toBeGreaterThan(before);
  });

  it('handles empty series gracefully', () => {
    const viz = buildStreamgraph([]);
    expect(viz.group.children).toHaveLength(0);
    expect(() => viz.tick(0)).not.toThrow();
  });

  it('reports the resolved category order', () => {
    const series = [
      { category: 'A', values: [1, 2, 3] },
      { category: 'B', values: [3, 2, 1] },
    ];
    const viz = buildStreamgraph(series);
    expect(viz.categories()).toEqual(['A', 'B']);
  });

  it('writeWindow scrolling visits different stack windows over time', () => {
    const series = sampleStreamgraph(3, 100);
    const viz = buildStreamgraph(series);
    const mesh = viz.group.children[0] as THREE.Mesh;
    const pos = mesh.geometry.getAttribute('position') as THREE.BufferAttribute;

    viz.tick(0);
    const first = (pos.array as Float32Array).slice();
    viz.tick(5); // 5 seconds @ default scrollSpeed = 8 → ~40 steps
    const later = (pos.array as Float32Array).slice();

    // At least some y-coordinates should differ — the wiggle baseline shifts as the stack window moves.
    let differences = 0;
    for (let i = 1; i < first.length; i += 3) {
      if (Math.abs(first[i]! - later[i]!) > 1e-5) differences++;
    }
    expect(differences).toBeGreaterThan(10);
  });
});
