declare module 'd3-force-3d' {
  export function forceSimulation<T = any>(nodes?: T[], numDimensions?: number): any;
  export function forceManyBody(): any;
  export function forceLink<T = any>(links?: T[]): any;
  export function forceCenter(x?: number, y?: number, z?: number): any;
  export function forceCollide(radius?: any): any;
  export function forceX(x?: number): any;
  export function forceY(y?: number): any;
  export function forceZ(z?: number): any;
}
