// Stage R2: the DATAFLOW between double slots, not the slots themselves.
//
// repr_slot_kernel.js measures the storage; this one measures what stage R2
// added on top of it. Every line of the loop body is a load out of a pinned
// slot, some arithmetic on what came out, and a store back into another pinned
// slot -- so a compiler that boxes between each link pays a canonicalizing
// select, a tag test and a GC root store per link, and one that keeps the
// value in an FP register pays none of them.
//
// The two objects alternate on purpose: a chain that stays inside one receiver
// could be held in registers by any compiler, but crossing between `a` and `b`
// puts a real store site between every pair of links.

class Cell {
  constructor(p, q, r) {
    this.p = p;
    this.q = q;
    this.r = r;
  }
}

// The NaN half of the checksum, kept out of the hot loop so it measures nothing
// and only has to be RIGHT. A double slot holds a raw f64, so a computed NaN
// arrives at the slot with whatever bit pattern the FPU produced; every one of
// these readers has to see the same NaN a boxed slot would have given, or the
// stage silently aliases a tag.
function nanScore() {
  const c = new Cell(0, 0, 0);
  let score = 0;
  const zero = Number('0');
  c.p = zero / zero;
  c.q = (1 / zero) - (1 / zero);
  c.r = c.p * 3.0 + c.q;
  if (Number.isNaN(c.p)) score += 1;
  if (Number.isNaN(c.q)) score += 2;
  if (Number.isNaN(c.r)) score += 4;
  if (Object.is(c.p, NaN)) score += 8;
  if (c.p !== c.p) score += 16;
  if (JSON.stringify({ v: c.p }) === '{"v":null}') score += 32;
  if (String(c.q) === 'NaN') score += 64;
  return score;
}

function run(iterations) {
  const a = new Cell(1.5, 2.5, 3.5);
  const b = new Cell(0.5, 1.25, 2.75);
  let acc = 0;
  // Every coefficient is under one and the only source term is `t`, so the
  // recurrence contracts: a chain this long that grew even slightly per
  // iteration would reach the infinities and the checksum would be NaN, which
  // pins nothing.
  for (let i = 0; i < iterations; i++) {
    const t = i * 1e-6;
    b.p = a.p * 0.5 + t;
    b.q = b.p * 0.25 + a.q * 0.5;
    b.r = b.q * 0.5 + a.r * 0.25 - t;
    a.p = b.r * 0.5 + b.p * 0.25;
    a.q = a.p * 0.5 + b.q * 0.25;
    a.r = a.q * 0.5 - a.p * 0.25 + t;
    acc += a.r - b.r;
  }
  console.log('repr_flow checksum=' + Math.round(acc * 1e6) + '/' + nanScore());
}

run(400000);
