// Ambient declarations for deps that ship no (or partial) types. Only the surface
// we use, so the bundle installs without @types packages.

declare module "qrcode" {
  export function toDataURL(text: string, opts?: unknown): Promise<string>;
  export function toCanvas(canvas: HTMLCanvasElement, text: string, opts?: unknown): Promise<void>;
  const _default: { toDataURL: typeof toDataURL; toCanvas: typeof toCanvas };
  export default _default;
}

declare module "jsqr" {
  export interface QRCode {
    data: string;
    binaryData: number[];
  }
  export default function jsQR(
    data: Uint8ClampedArray,
    width: number,
    height: number,
    opts?: { inversionAttempts?: "dontInvert" | "onlyInvert" | "attemptBoth" | "invertFirst" },
  ): QRCode | null;
}
