// Kernel isolation: a THREE-DEEP chain of pinned-signature method calls, the
// shape three.js sets every uniform through — `setValue` asks a cache-compare
// and then a cache-store, and both take the same four numbers it did.
// (`WebGLUniforms.setValueV4f` is this function with the two halves written
// out; the chain is what the array and matrix setters beside it look like.)
//
// It is the case the typed calling convention exists for, and the case
// `mat4_kernel` is not. `Matrix4.multiplyMatrices` is one call around a
// hundred flops, so its boundary is a few percent of it; this is three calls
// around eight compares and four stores, so the boundary IS the work. And what
// the boundary costs is not the argument vector — that is half a nanosecond —
// but the callee's own prologue: on Windows x64 a float-heavy method spills
// callee-saved XMM registers, pushes a GC root frame, fetches its thread's ABI
// block, and re-derives every field it reads through a guard the caller
// already established. Three of those per uniform.
//
// Both halves are timed here so the fixture carries its own ceiling: `chained`
// makes the calls, `flat` is the same arithmetic written out in one method.
// With `--pins bench/pins/call-chain-kernel.pins` the two converge — a direct
// edge to a typed entry that LLVM may inline is what closes the gap — and the
// RATIO is the measurement, not either number alone.
//
// Node-oracled: the two loops answer the same checksums under node, and node's
// own ratio (~1.0) is the floor the ratio is read against.

import { measure } from './harness.js';

const ITERS = 8000000;

class Uniform {
  constructor() {
    this.c0 = 0;
    this.c1 = 0;
    this.c2 = 0;
    this.c3 = 0;
    this.writes = 0;
  }

  // Depth 2: the cache compare that decides whether depth 3 runs at all.
  same(x, y, z, w) {
    if (this.c0 !== x) return 0;
    if (this.c1 !== y) return 0;
    if (this.c2 !== z) return 0;
    if (this.c3 !== w) return 0;
    return 1;
  }

  // Depth 3: the cache store.
  store(x, y, z, w) {
    this.c0 = x;
    this.c1 = y;
    this.c2 = z;
    this.c3 = w;
    return x + y + z + w;
  }

  // Depth 1: the setter.
  setValue(x, y, z, w) {
    if (this.same(x, y, z, w) === 1) return 0;
    this.writes = this.writes + 1;
    return this.store(x, y, z, w);
  }

  // The same work with the two inner calls written out: the ceiling the
  // chained form is measured against.
  setValueFlat(x, y, z, w) {
    if (this.c0 === x && this.c1 === y && this.c2 === z && this.c3 === w) return 0;
    this.writes = this.writes + 1;
    this.c0 = x;
    this.c1 = y;
    this.c2 = z;
    this.c3 = w;
    return x + y + z + w;
  }
}

function chained(iters) {
  const u = new Uniform();
  let acc = 0;
  for (let i = 0; i < iters; i++) {
    const k = i & 15;
    acc = acc + u.setValue(k, k + 1, k + 2, k + 3);
  }
  return acc + u.writes;
}

function flat(iters) {
  const u = new Uniform();
  let acc = 0;
  for (let i = 0; i < iters; i++) {
    const k = i & 15;
    acc = acc + u.setValueFlat(k, k + 1, k + 2, k + 3);
  }
  return acc + u.writes;
}

// Two regions, timed and reported apart, because what this kernel measures is
// the RATIO between them: `chained` pays a typed call edge per link and `flat`
// does the same arithmetic inline, so one combined number would destroy the
// comparison. `bench/tools/interleave.py` divides the two ns_per_iter figures.
const a = measure('call_chain_kernel.chained', () => chained(ITERS), ITERS);
const b = measure('call_chain_kernel.flat', () => flat(ITERS), ITERS);
console.log(`call_chain iters=${ITERS} chained=${a} flat=${b}`);
