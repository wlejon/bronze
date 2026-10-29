// A toArray-shaped run `array[offset + k] = te[k]` from a dense array source:
// the batched lowering tests every source element, then loads each one again
// in the store block. These cases pin what JS order gives when the target is
// the source itself (each read sees the stores before it), when a source
// element is not a Number or is a hole (the helper path), and for each kind
// of target.
class M {
  constructor() { this.elements = [1, 2, 3, 4, 5, 6]; }
  toArray(array, offset) {
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

function show(a) { return Array.from(a).map(String).join(","); }

const m = new M();
for (let i = 0; i < 3; i++) {
  console.log("plain", show(m.toArray([], 0)));
  console.log("f32", show(m.toArray(new Float32Array(8), 1)));
  console.log("f64", show(m.toArray(new Float64Array(7), 1)));
  console.log("dense", show(m.toArray([0, 0, 0, 0, 0, 0, 0, 0], 2)));
}

// The target is the source, shifted by one: te[k] is read after
// array[offset + k - 1] = te[k - 1] stored into it, so the first element
// spreads along the whole run.
for (let i = 0; i < 3; i++) {
  const a = new M();
  a.elements.push(0);
  console.log("alias", show(a.toArray(a.elements, 1)));
}

// A non-Number and a hole in the source take the helper path.
const s = new M();
s.elements[3] = "x";
console.log("string", show(s.toArray(new Float32Array(6), 0)));
const h = new M();
delete h.elements[2];
console.log("hole", show(h.toArray([0, 0, 0, 0, 0, 0], 0)));
console.log("hole f64", show(h.toArray(new Float64Array(6), 0)));
const f = new M();
f.elements[1] = 0.1;
f.elements[5] = NaN;
console.log("fraction", show(f.toArray(new Float32Array(6), 0)));
console.log("fraction f64", show(f.toArray(new Float64Array(6), 0)));
