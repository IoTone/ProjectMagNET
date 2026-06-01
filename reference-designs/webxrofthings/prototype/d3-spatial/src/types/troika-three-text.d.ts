declare module 'troika-three-text' {
  import { Mesh } from 'three';
  export class Text extends Mesh {
    text: string;
    fontSize: number;
    color: number | string;
    anchorX: 'left' | 'center' | 'right' | number | string;
    anchorY: 'top' | 'top-baseline' | 'middle' | 'bottom-baseline' | 'bottom' | number | string;
    font?: string;
    outlineWidth?: number | string;
    outlineColor?: number | string;
    maxWidth?: number;
    sync(cb?: () => void): void;
    dispose(): void;
  }
}
