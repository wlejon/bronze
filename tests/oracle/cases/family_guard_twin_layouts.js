// Two classes that extend nothing and declare the same field list, the way
// three.js's Matrix3 and Matrix4 both declare `elements`. Either class's
// field list describes either class's instances, and a shape carries one
// family stamp, so which class a shape is stamped with depends on which site
// asked first. Whatever it is, every site must read and write the property it
// names: a site whose class lost the tie misses into the inline cache, and a
// site that meets the OTHER class's instance (passed in through a method that
// does not care) reads the same slot by name.

class Mat3 {
  constructor() {
    this.elements = [1, 0, 0, 0, 1, 0, 0, 0, 1];
  }
  trace() {
    const e = this.elements;
    return e[0] + e[4] + e[8];
  }
  scale(s) {
    const e = this.elements;
    for (let i = 0; i < e.length; i++) e[i] *= s;
    return this;
  }
}

class Mat4 {
  constructor() {
    this.elements = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  }
  trace() {
    const e = this.elements;
    return e[0] + e[5] + e[10] + e[15];
  }
  // Reads `elements` off two other receivers, the way multiplyMatrices does.
  sum(a, b) {
    const ae = a.elements;
    const be = b.elements;
    const te = this.elements;
    for (let i = 0; i < te.length; i++) te[i] = (ae[i] || 0) + (be[i] || 0);
    return this;
  }
  swap(other) {
    const t = this.elements;
    this.elements = other.elements;
    other.elements = t;
    return this;
  }
}

// Mat4's sites run first here, so its instances' shape is asked about by its
// own sites before Mat3's are.
const m4 = new Mat4();
const n4 = new Mat4();
let acc = 0;
for (let k = 0; k < 50; k++) {
  m4.sum(m4, n4);
  acc += m4.trace();
}
console.log("mat4 trace", m4.trace(), "acc", acc);

const m3 = new Mat3();
for (let k = 0; k < 50; k++) m3.scale(1.01);
console.log("mat3 trace", m3.trace().toFixed(6));

// A Mat3 handed to Mat4's sites: same field, other class, other shape.
const p4 = new Mat4();
p4.sum(m3, n4);
console.log("mixed sum", p4.elements.slice(0, 9).map((x) => x.toFixed(3)).join(","));

// The two layouts' elements swapped through Mat4's write site: Mat3's trace
// now reads a 16-element array at its own indices.
const q4 = new Mat4();
const q3 = new Mat3();
q4.swap(q3);
console.log("swapped", q4.elements.length, q3.elements.length, q3.trace(), q4.trace());

// An object literal with the same one property, at its own shape again.
const lit = { elements: [2, 2, 2, 2, 2, 2, 2, 2, 2] };
console.log("literal", Mat3.prototype.trace.call(lit), Mat4.prototype.trace.call(lit));
