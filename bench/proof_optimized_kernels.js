// Runtime benchmark comparing original Three.js + Pixi.js math/color/batching
// kernels against their formally proven superoptimized versions produced by
// `rewriteJsSource` (Horner polynomial evaluation, Math.pow(x, 2/3) strength
// reduction, Math.floor(u / 2^k) shift reduction, and CEGIS BGR/ABGR bit-field
// superoptimization).

import { measure } from './harness.js';

function runOriginalKernels(n) {
  const tints = new Int32Array(256);
  for (let i = 0; i < 256; i++) {
    tints[i] = ((i * 73) & 255) << 16 | ((i * 151) & 255) << 8 | ((i * 211) & 255);
  }
  let acc = 0;
  for (let iter = 0; iter < n; iter++) {
    const idx = iter & 255;
    const rgb = tints[idx] | 0;
    const u = idx & 15;
    // 1. Pixi.js Color.toBgrNumber / packPremultipliedAlpha
    const bgr = ((rgb >> 16) & 255) | (((rgb >> 8) & 255) << 8) | ((rgb & 255) << 16);
    // 2. Three.js / Pixi.js cubic Hermite / polynomial curve evaluation
    const poly = (2 * Math.pow(u, 3) + 3 * Math.pow(u, 2) + 4 * u + 5) | 0;
    // 3. Three.js Vector3.lengthSq + channel quantization
    const vx = (idx & 31) - 15;
    const vy = ((idx >> 2) & 31) - 15;
    const vz = ((idx >> 3) & 31) - 15;
    const lenSq = (Math.pow(vx, 2) + Math.pow(vy, 2) + Math.pow(vz, 2)) | 0;
    const quant = Math.floor((idx & 255) / 16) | 0;
    acc = (acc + (bgr ^ poly ^ lenSq ^ quant)) | 0;
  }
  return acc;
}

function runProofOptimizedKernels(n) {
  const tints = new Int32Array(256);
  for (let i = 0; i < 256; i++) {
    tints[i] = ((i * 73) & 255) << 16 | ((i * 151) & 255) << 8 | ((i * 211) & 255);
  }
  let acc = 0;
  for (let iter = 0; iter < n; iter++) {
    const idx = iter & 255;
    const rgb = tints[idx] | 0;
    const u = idx & 15;
    // 1. CEGIS superoptimized Pixi.js BGR bit-packing: `(((rgb >> 8) & 255) << 8)` -> `(rgb & 65280)`
    const bgr = ((rgb >> 16) & 255) | (rgb & 65280) | ((rgb & 255) << 16);
    // 2. Proven Horner polynomial factoring: `(((2 * u + 3) * u + 4) * u + 5)`
    const poly = (((2 * u + 3) * u + 4) * u + 5) | 0;
    // 3. Proven strength-reduced squares & shift quantization
    const vx = (idx & 31) - 15;
    const vy = ((idx >> 2) & 31) - 15;
    const vz = ((idx >> 3) & 31) - 15;
    const lenSq = ((vx * vx) + (vy * vy) + (vz * vz)) | 0;
    const quant = (idx & 255) >> 4;
    acc = (acc + (bgr ^ poly ^ lenSq ^ quant)) | 0;
  }
  return acc;
}

const ITERS = 2000000;
const origChecksum = measure('original_kernels', () => runOriginalKernels(ITERS), ITERS);
const optChecksum = measure('proof_optimized_kernels', () => runProofOptimizedKernels(ITERS), ITERS);

console.log('original_kernels checksum=' + origChecksum);
console.log('proof_optimized_kernels checksum=' + optChecksum);
if (origChecksum !== optChecksum) {
  throw new Error('Checksum mismatch: ' + origChecksum + ' !== ' + optChecksum);
}
