declare module 'omnitone' {
  export function createFOARenderer(ctx: AudioContext, opts?: any): any;
  export function createHOARenderer(ctx: AudioContext, opts?: any): any;
  const _default: { createFOARenderer: typeof createFOARenderer; createHOARenderer: typeof createHOARenderer };
  export default _default;
}
declare module 'omnitone/build/omnitone.esm.js' {
  const o: { createFOARenderer: (ctx: AudioContext, opts?: any) => any; createHOARenderer: (ctx: AudioContext, opts?: any) => any };
  export default o;
}
