/**
 * Minimal ambient declarations for `@mkkellogg/gaussian-splats-3d`. The
 * package ships JavaScript without TypeScript types; this file declares
 * just the surface our splat-gallery cell consumes.
 */
declare module '@mkkellogg/gaussian-splats-3d' {
  import { Group } from 'three';

  export interface AddSplatSceneOptions {
    /** Drop splats below this alpha threshold. Higher = fewer splats, less
     *  haloing. mkkellogg's recommended starting point is 5. */
    splatAlphaRemovalThreshold?: number;
    /** Whether the viewer should show its own loading UI overlay. */
    showLoadingUI?: boolean;
    /** Position/rotation/scale applied to the scene at load time. */
    position?: [number, number, number];
    rotation?: [number, number, number, number];
    scale?: [number, number, number];
    /** Use a progressive loader (lower memory but slower interactive). */
    progressiveLoad?: boolean;
  }

  export interface DropInViewerOptions {
    /** Whether to use a SharedArrayBuffer for cross-thread state.
     *  Requires COOP/COEP headers on the host; safer to leave off in dev. */
    sharedMemoryForWorkers?: boolean;
    /** Tells the splat sorter to run on the GPU when available. */
    gpuAcceleratedSort?: boolean;
    /** Other options exist in the upstream Viewer config; widen as needed. */
    [k: string]: unknown;
  }

  /** mkkellogg's "drop-in" Viewer: a `THREE.Group` that takes care of its
   *  own update + render path. Add to any three.js scene like any object. */
  export class DropInViewer extends Group {
    constructor(opts?: DropInViewerOptions);
    addSplatScene(path: string, opts?: AddSplatSceneOptions): Promise<void>;
    addSplatScenes(scenes: Array<AddSplatSceneOptions & { path: string }>): Promise<void>;
    removeSplatScene(index: number, showLoadingUI?: boolean): Promise<void>;
    removeSplatScenes(indexes: number[], showLoadingUI?: boolean): Promise<void>;
    dispose(): void;
  }
}
