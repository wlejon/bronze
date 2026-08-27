// Kernel isolation: Matrix4.multiplyMatrices ns/call, via the real vendored
// three.js class. It times its own loop through bench/harness.js and reports
// ns_per_iter over 20M calls. Used by the BRONZE_UNSOUND_PINS ceiling probe.

import { Matrix4 } from '../tests/oracle/threejs/three/math/Matrix4.js';
import { measure } from './harness.js';

const ITERS = 20000000;

function run(iters) {
  const a = new Matrix4();
  const b = new Matrix4();
  const c = new Matrix4();
  a.set(1.1, 0.2, 0.3, 0.4, 0.5, 1.6, 0.7, 0.8, 0.9, 0.1, 1.2, 0.3, 0.0, 0.0, 0.0, 1.0);
  b.set(0.9, 0.1, 0.2, 0.3, 0.4, 0.8, 0.5, 0.6, 0.7, 0.2, 1.1, 0.4, 0.0, 0.0, 0.0, 1.0);
  let acc = 0.0;
  for (let i = 0; i < iters; i++) {
    c.multiplyMatrices(a, b);
    acc += c.elements[5];
    // Perturb one input so the multiply cannot be hoisted out of the loop.
    a.elements[0] = 1.0 + (i & 7) * 0.125;
  }
  return acc;
}

const acc = measure('mat4_kernel', () => run(ITERS), ITERS);
console.log(`mat4_kernel iters=${ITERS} checksum=${Math.round(acc % 1000000)}`);
