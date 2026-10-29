// A toArray-shaped run that reads its whole dense source, element k at step
// k, tests the source's NUMBERS bit (runtime/array.h) instead of each element
// once the bit is set; the first run whose elements all test as Numbers sets
// it (il2mir/il_lowering_elem_run.cpp). These cases pin that the bit's answer
// is never stale: a store of a non-Number into a claimed source, a source
// grown, shrunk or holed after its claim, -0 and NaN
// and Infinity (Numbers all), and the targets and offsets the run's own fast
// path refuses (a short view, a view subclass, a string or fractional offset).
class M {
  constructor() { this.elements = [1, 2, 3, 4, 5, 6]; }
  toArray(array = [], offset = 0) {
    const te = this.elements;
    array[offset] = te[0];
    array[offset + 1] = te[1];
    array[offset + 2] = te[2];
    array[offset + 3] = te[3];
    array[offset + 4] = te[4];
    array[offset + 5] = te[5];
    return array;
  }
}

class F32 extends Float32Array {}

function num(x) { return Object.is(x, -0) ? "-0" : String(x); }
function show(a) { return Array.from(a).map(num).join(","); }
function keys(a) { return Object.keys(a).map((k) => k + "=" + num(a[k])).join(","); }

// Each call twice in a loop: the first sets the bit, the second reads it.
function twice(m, make, offset, label) {
  let out = "";
  for (let i = 0; i < 2; i++) out = show(m.toArray(make(), offset));
  console.log(label, out);
}

const m = new M();
twice(m, () => [], 0, "plain");
twice(m, () => new Float32Array(8), 1, "f32");
twice(m, () => new Float64Array(7), 1, "f64");
twice(m, () => new F32(6), 0, "sub");
twice(m, () => new Float32Array(4), 0, "short");
console.log("default", show(m.toArray()));

// After the claim: a non-Number stored into the source clears the bit.
m.elements[2] = "y";
twice(m, () => [], 0, "string");
twice(m, () => new Float64Array(6), 0, "string f64");
m.elements[2] = "5";
m.elements[3] = true;
twice(m, () => new Float32Array(6), 0, "coerce f32");
m.elements[2] = 3;
m.elements[3] = 4;
twice(m, () => [], 0, "back");

// Numbers that are not ordinary: the bit is about Numbers, all of them.
const odd = new M();
odd.elements[0] = -0;
odd.elements[1] = NaN;
odd.elements[2] = Infinity;
odd.elements[3] = 2 ** 31;
twice(odd, () => [], 0, "odd");
twice(odd, () => new Float32Array(6), 0, "odd f32");

// Grown after its claim (read up to 5 still), and shrunk below the run.
const g = new M();
twice(g, () => [], 0, "before");
g.elements.push(7);
twice(g, () => [], 0, "grown");
g.elements.length = 3;
twice(g, () => [], 0, "shrunk");

// A hole punched after the claim.
const a = new M();
twice(a, () => [], 0, "plain2");
delete a.elements[4];
twice(a, () => [], 0, "hole");
twice(a, () => new Float64Array(6), 0, "hole f64");

// Offsets that are no index: a string concatenates, a fraction names no
// element of a view (so its stores do nothing).
const o = new M();
console.log("string offset", keys(o.toArray([], "2")));
console.log("fraction offset", keys(o.toArray([], 0.5)));
console.log("fraction f32", show(o.toArray(new Float32Array(8), 0.5)));

// The target is the source.
const self = new M();
twice(self, () => self.elements, 0, "self");
