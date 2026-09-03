// Array destructuring of a small tuple a function returns — `const [x, z] =
// cellCenter(cx, cy)` — the hex-geometry idiom a sim's motion, combat and
// pathfinding phases pay on every distance test.
//
// Each destructuring is `pattern.check` + `iter.open` + N steps + `iter.close`
// over a two-element array. The open and the steps were inline already; the
// check and the close were helper calls that did nothing for an array
// (rt_spread.cpp: the check raises only for null/undefined; iterator.cpp: the
// close returns at once for a cursor kind), and they were 39 % of the sim's
// dynamic helper bill by count. Both are now inline tests with the helper on
// the raising edge only.
//
// `tuple_index` is the control: the same arithmetic reading `p[0]` and `p[1]`,
// which is what the destructuring should cost and no more.

import { measure } from './harness.js';

const N = 3000000;
const SQ3 = Math.sqrt(3);

function cellCenter(cx, cy) {
  const x = SQ3 * (cx + 0.5 * (cy & 1));
  const z = 1.5 * cy;
  return [x, z];
}

function destructure() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const [x, z] = cellCenter(i & 63, (i >> 6) & 63);
    const [x2, z2] = cellCenter((i + 1) & 63, (i >> 5) & 63);
    const dx = x - x2, dz = z - z2;
    acc += dx * dx + dz * dz;
  }
  return acc;
}

function tupleIndex() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const p = cellCenter(i & 63, (i >> 6) & 63);
    const q = cellCenter((i + 1) & 63, (i >> 5) & 63);
    const dx = p[0] - q[0], dz = p[1] - q[1];
    acc += dx * dx + dz * dz;
  }
  return acc;
}

function nestedDestructure() {
  let acc = 0;
  const pts = [];
  for (let i = 0; i < 64; i++) pts.push([i, i * 2, [i & 1, i & 2]]);
  for (let i = 0; i < N / 64; i++) {
    for (const [a, b, [c, d]] of pts) acc += a + b + c + d;
  }
  return acc;
}

const a = measure('array_destructure', destructure, N);
const b = measure('tuple_index', tupleIndex, N);
const c = measure('nested_destructure_forof', nestedDestructure, N);
console.log('checksum ' + (a + b + c).toFixed(3));
