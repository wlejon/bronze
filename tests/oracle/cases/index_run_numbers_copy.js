// A copy run (`te[0] = me[0]; ... te[15] = me[15]`, Matrix4.copy) whose
// stores are all its own reads of one dense source copies without hole tests
// or barriers while the source's NUMBERS bit (runtime/array.h) is set, and a
// tested pass over a whole 16-element source with no properties object claims
// the bit for the next visit (il2mir/il_lowering_index_run.cpp). These cases
// pin that the bit's answer is never stale and that every source the bit path
// must not take still copies exactly: non-Numbers stored into a claimed
// source, holes, grown and shrunk sources, named
// properties, fill and splice, odd Numbers, objects, typed, frozen and
// non-array targets, and a self copy.
class M4 {
  constructor() {
    this.elements = [];
    for (let i = 0; i < 16; i++) this.elements.push(i * 1.5);
  }
  copy(m) {
    const te = this.elements;
    const me = m.elements;
    te[0] = me[0]; te[1] = me[1]; te[2] = me[2]; te[3] = me[3];
    te[4] = me[4]; te[5] = me[5]; te[6] = me[6]; te[7] = me[7];
    te[8] = me[8]; te[9] = me[9]; te[10] = me[10]; te[11] = me[11];
    te[12] = me[12]; te[13] = me[13]; te[14] = me[14]; te[15] = me[15];
    return this;
  }
}

function num(x) {
  if (typeof x === "number") return Object.is(x, -0) ? "-0" : String(x);
  if (typeof x === "object" && x !== null) return "obj";
  return String(x);
}
function show(a) {
  const out = [];
  for (let i = 0; i < 16; i++) out.push(i in a ? num(a[i]) : "hole");
  return out.join(",");
}

// Each copy twice in a loop: the first may claim the bit, the second reads it.
function twice(dst, src, label) {
  for (let i = 0; i < 2; i++) dst.copy(src);
  console.log(label, show(dst.elements));
}

const src = new M4();
const dst = new M4();
for (let i = 0; i < 16; i++) dst.elements[i] = -1;
twice(dst, src, "basic");

// A Number stored into the claimed source keeps the bit; the copy sees it.
src.elements[3] = 99;
twice(dst, src, "changed");

// A non-Number stored into the claimed source clears the bit.
src.elements[5] = "x";
twice(dst, src, "string");
src.elements[5] = 7.5;
twice(dst, src, "back");

// An object element, then a Number again: identity is kept.
const o = { k: 1 };
src.elements[2] = o;
dst.copy(src);
dst.copy(src);
console.log("object", dst.elements[2] === o, show(dst.elements));
src.elements[2] = 3;
twice(dst, src, "object gone");

// Numbers that are not ordinary are Numbers all the same.
src.elements[0] = -0;
src.elements[1] = NaN;
src.elements[4] = Infinity;
src.elements[6] = -Infinity;
src.elements[8] = 7 | 0;
twice(dst, src, "odd");

// A hole reads as undefined and the copy stores undefined, an own element.
const holed = new M4();
delete holed.elements[7];
twice(dst, holed, "hole");
console.log("hole own", 7 in dst.elements, dst.elements[7] === undefined);

// (An inherited index answering a hole's read is not reachable: bronze
// refuses an index defined on Array.prototype, "Cannot decorate".)
holed.elements[7] = 10.5;
twice(dst, holed, "filled");

// Grown past the reads: the leading 16 are copied, the source keeps its length.
const grown = new M4();
dst.copy(grown);
grown.elements.length = 20;
twice(dst, grown, "grown");
console.log("grown len", grown.elements.length, 17 in grown.elements);
grown.elements.length = 16;
twice(dst, grown, "regrown");

// Shrunk below the reads: the missing ones read undefined.
const shrunk = new M4();
dst.copy(shrunk);
shrunk.elements.length = 10;
twice(dst, shrunk, "shrunk");

// A named property on the source: the elements still copy.
const named = new M4();
dst.copy(named);
named.elements.extra = "e";
named.elements[9] = 100;
twice(dst, named, "named");
console.log("named keys", Object.keys(named.elements).length);

// fill and splice on a claimed source.
const filled = new M4();
dst.copy(filled);
filled.elements.fill("s", 4, 6);
twice(dst, filled, "fill");
filled.elements.fill(2, 4, 6);
twice(dst, filled, "refill");
filled.elements.splice(0, 1);
twice(dst, filled, "splice");

// The same array on both sides.
const self = new M4();
self.copy(self);
self.copy(self);
console.log("self", show(self.elements));

// Targets the run's guard refuses.
const typed = new M4();
typed.elements = new Float64Array(16);
twice(typed, src, "typed");
const plainSrc = new M4();
plainSrc.elements = {};
for (let i = 0; i < 16; i++) plainSrc.elements[i] = 100 + i;
twice(dst, plainSrc, "object source");
const frozen = new M4();
Object.freeze(frozen.elements);
try {
  frozen.copy(src);
  console.log("frozen", "no throw");
} catch (e) {
  console.log("frozen", e.name, frozen.elements[0], frozen.elements[15]);
}
