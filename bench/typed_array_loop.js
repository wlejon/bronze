// The loop the indexed fast path is about: element access on a typed array,
// which is what three.js's `BufferAttribute` inner loops are made of.
//
// The two halves are deliberately comparable: the same arithmetic runs over a
// Float32Array and over a plain JS array, so the difference between the two
// numbers is the difference between the two element paths and not the loop.
// They are timed as SEPARATE regions for that reason — one combined number
// would destroy the comparison the file exists to make.
//
// The two paths have swapped sides since this file was written. The typed half
// is now the faster of the two under bronze and the SLOWER of the two under
// node; the plain-array element path is the one left behind.
import { measure } from './harness.js';

function overTypedArray(n) {
  const v = new Float32Array(1024);
  for (let i = 0; i < 1024; i++) v[i] = i * 0.5;
  let sum = 0;
  for (let pass = 0; pass < n; pass++) {
    for (let i = 0; i < 1024; i++) {
      sum = sum + v[i];
      v[i] = v[i] * 1.0000001;
    }
  }
  return sum;
}

function overPlainArray(n) {
  const a = [];
  for (let i = 0; i < 1024; i++) a.push(i * 0.5);
  let sum = 0;
  for (let pass = 0; pass < n; pass++) {
    for (let i = 0; i < 1024; i++) {
      sum = sum + a[i];
      a[i] = a[i] * 1.0000001;
    }
  }
  return sum;
}

console.log(measure('typed_array_loop.typed',
                    () => overTypedArray(2000), 2000 * 1024));
console.log(measure('typed_array_loop.plain',
                    () => overPlainArray(2000), 2000 * 1024));
