// Kernel isolation: a field that holds a number OR null OR undefined — the
// `--pins number-or-nullish` shape (src/types/pins.h). Uninitialized slots and
// optional constructor arguments stored on `this` are what three.js is full of,
// and the flat lattice types every one of them `Dynamic`.
//
// Four sweeps over ONE compiled `step`, so the pinned read is exercised in
// every state the pin promises:
//
//   numbers  — the slot holds doubles; the pinned read is a raw f64.
//   nulls    — ToNumber(null) is +0 and the relational is an ordered compare
//              against +0, so this sweep's checksum is FINITE. It is the arm
//              that separates "the nullish path implements ToNumber" from
//              "the nullish path answers NaN for everything".
//   undefs   — ToNumber(undefined) is NaN, so the accumulator is NaN and every
//              `<` against it is false. NaN is the correct answer here and it
//              is the strongest oracle in the file: a read that took the
//              undefined singleton's BITS as a double would produce a large
//              finite number, not NaN.
//   mixed    — both arms in one loop, so the compare is not predicted away.
//
// It times its own loop through bench/harness.js and reports ns_per_iter over
// the four sweeps the region covers.

import { measure } from './harness.js';

const ITERS = 8000000;

class Node {
  constructor(limit) {
    this.x = 0.5;
    this.y = 0.0;
    this.below = 0.0;
    // A number, a null, or an undefined — whatever the caller passed.
    this.limit = limit;
  }

  step(t) {
    this.x = this.x * 0.9997 + t;
    if (this.x > 1.0) this.x = this.x - 1.0;
    // Coercing position: the pinned read feeds a `*` and a `-`.
    this.y = this.y + this.x * 0.5 - this.limit * 0.25;
    // Relational position: `<` is ToNumber on both sides too.
    if (this.x < this.limit) this.below = this.below + 1.0;
    return this.y;
  }
}

function sweep(node, iters) {
  let acc = 0.0;
  for (let i = 0; i < iters; i++) {
    acc += node.step(i * 1e-9);
  }
  return acc + node.below;
}

function run(iters) {
  const numbers = sweep(new Node(0.75), iters);
  const nulls = sweep(new Node(null), iters);
  const undefs = sweep(new Node(undefined), iters);
  // Alternating instances: the same `step` sees both arms, iteration by
  // iteration, so neither is the only one the branch predictor ever learns.
  const a = new Node(0.6);
  const b = new Node(null);
  let mixedAcc = 0.0;
  for (let i = 0; i < iters; i++) {
    mixedAcc += (i & 1) === 0 ? a.step(1e-9) : b.step(1e-9);
  }
  return [numbers, nulls, undefs, mixedAcc + a.below + b.below];
}

const [numbers, nulls, undefs, mixed] =
  measure('nullish_pin_kernel', () => run(ITERS), ITERS * 4);
console.log(
  `nullish_pin iters=${ITERS} checksum=${Math.round(numbers % 1000000)}/` +
  `${Math.round(nulls % 1000000)}/${Math.round(undefs)}/${Math.round(mixed % 1000000)}`);
