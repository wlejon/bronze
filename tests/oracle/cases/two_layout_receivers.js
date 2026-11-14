// Objects of one class in two layouts (a field one constructor path adds and
// the other does not) reach the same reads, stores and typed-array loops,
// alternating: a site claimed for one layout must answer the other, a store
// must land in the right slot of each, and a loop over a view the object
// holds must read the view it was handed, whatever its length.
class Mixer {
  constructor(size, quat, i) {
    this.binding = null;
    this.valueSize = size;
    this.buffer = new Float64Array(size * 5);
    if (quat) this._workIndex = 5;
    this.cumulativeWeight = 0;
    this.tag = null;
    for (let k = 0; k < size * 5; k++) this.buffer[k] = k * 0.25 + i;
  }
  accumulate(accuIndex, weight) {
    const buffer = this.buffer, stride = this.valueSize, offset = accuIndex * stride + stride;
    let currentWeight = this.cumulativeWeight;
    if (currentWeight === 0) {
      for (let i = 0; i !== stride; ++i) buffer[offset + i] = buffer[i];
      currentWeight = weight;
    } else {
      currentWeight += weight;
    }
    this.cumulativeWeight = currentWeight;
  }
  apply(accuIndex) {
    const stride = this.valueSize, buffer = this.buffer, offset = accuIndex * stride + stride;
    const weight = this.cumulativeWeight;
    this.cumulativeWeight = 0;
    let changed = 0;
    for (let i = stride, e = stride + stride; i !== e; ++i) {
      if (buffer[i] !== buffer[i + stride]) { changed = 1; break; }
    }
    return changed + offset + weight;
  }
  mark(v) { this.tag = v; }  // an object or a number into a slot whose index differs by layout
}
const mixers = [];
for (let i = 0; i < 400; i++) mixers.push(new Mixer(i % 11 === 0 ? 3 : 4, i % 11 !== 0, i));
let total = 0;
const marks = [{ id: 1 }, 2.5, "three", null];
for (let f = 0; f < 300; f++) {
  for (let i = 0; i < mixers.length; i++) {
    const m = mixers[i];
    m.buffer[0] = f + i;
    m.accumulate(f & 1, 1);
    if ((f + i) % 7 === 0) m.accumulate(f & 1, 0.5);
    total += m.apply(f & 1);
    m.mark(marks[(f + i) & 3]);
  }
}
console.log("total", total);
let tags = 0;
for (const m of mixers) {
  tags += typeof m.tag === "object" ? (m.tag === null ? 1 : m.tag.id * 10) : typeof m.tag === "number" ? m.tag : m.tag.length;
  if (m._workIndex !== undefined && m._workIndex !== 5) console.log("work index overwritten", m._workIndex);
}
console.log("tags", tags, mixers[0].cumulativeWeight, mixers[1].cumulativeWeight, mixers[0].tag, mixers[1].tag);

// A counter that runs past the view: every read past the end is undefined and
// every store past it is dropped, for a bound far beyond the length and for
// one the counter never equals until it is past it.
function sweep(view, n) {
  let seen = 0, holes = 0;
  for (let i = 0; i !== n; ++i) {
    const v = view[i];
    if (v === undefined) holes++; else seen += v;
    view[i + 2] = i;
  }
  return seen + "/" + holes;
}
const small = new Float64Array(6);
for (let r = 0; r < 200; r++) sweep(small, 6);
console.log("sweep", sweep(small, 6), sweep(small, 11), sweep(new Float64Array(3), 40), Array.from(small).join(","));
// The same loop over a Float32Array and an Array, after the Float64Array runs.
console.log("kinds", sweep(new Float32Array(5), 9), sweep([1, 2, 3], 5));
