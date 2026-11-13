// A run of constant-index stores of computed doubles stores their bits after
// one NaN test of their sum (il_lowering_index_run.cpp, "ONE NaN TEST FOR THE
// RUN"). A NaN made by inf - inf or 0 * inf (x86's default NaN, which as bits
// is above every Number and would read as a tagged value) must still land as
// the canonical NaN, in an array element and in an object field, and read
// back as a Number everywhere it goes.
class Vec { constructor(x, y, z) { this.x = x; this.y = y; this.z = z; } }
class Box {
  constructor() { this.e = [0, 0, 0, 0, 0, 0, 0, 0]; this.n = 0; }
  fill(p, s) {
    const e = this.e;
    const a = p.x * s, b = p.y - s, c = p.z * p.x;
    e[0] = a; e[1] = b; e[2] = c; e[3] = 0;
    e[4] = a + b; e[5] = 1; e[6] = p.x; e[7] = p.y;
    this.n = b;
    return this;
  }
}

function show(v) {
  return (typeof v) + ":" + (v !== v ? "NaN" : String(v)) + ":" + Object.is(v, NaN) + ":" + (v === v);
}
function viaCall(v) { return v; }
function describe(box) {
  const out = [];
  for (let i = 0; i < box.e.length; i++) {
    const v = viaCall(box.e[i]);
    out.push(show(v) + (isNaN(v) ? "" : "=" + (v + 1)));
  }
  out.push("n " + show(viaCall(box.n)));
  out.push("sum " + show(box.e.reduce((s, v) => s + v, 0)));
  return out.join(" | ");
}

const inf = Infinity;
const boxes = [];
for (let i = 0; i < 8; i++) boxes.push(new Box());
// Every input goes through one call site, so the special ones meet the run's
// fast path that the ordinary ones before them made hot: inf - inf and
// 0 * inf (NaN terms), inf and -inf (a NaN sum with no NaN term), all NaN,
// then finite again on a box that held NaNs.
const special = [[0, inf, inf, inf], [inf, -inf, 1, 1], [NaN, NaN, NaN, NaN], [1, 2, 3, 4]];
const N = 20000;
let acc = 0;
for (let r = 0; r < N + special.length; r++) {
  const k = r - N;
  const bx = boxes[k >= 0 ? k : r & 7];
  const sp = k >= 0 ? special[k] : null;
  if (sp) bx.fill(new Vec(sp[0], sp[1], sp[2]), sp[3]);
  else bx.fill(new Vec(r * 0.5, 2.5, 3), 1.5);
  if (sp) console.log("ABCD"[k], describe(bx));
  else acc += bx.e[4];
}
console.log("warm", acc);

// The NaN from a store compared and used as keys.
const nb = boxes[2].e;
console.log("cmp", nb[0] == nb[0], nb[0] < 1, nb[0] > 1, Math.max(nb[1], 1), [nb[2]].includes(NaN),
            [nb[2]].indexOf(NaN), new Set([nb[0], nb[1], NaN]).size);
const o = { v: boxes[0].e[1] };
o.v = 0 * inf;
console.log("field", show(o.v), show(viaCall(o).v), JSON.stringify(o));
