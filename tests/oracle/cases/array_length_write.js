// `a.length = n` — ECMA-262 10.4.2.4 ArraySetLength, the write that makes an
// array an exotic object.
//
// `length` is not a named property and not an element: it is the one own
// property whose [[Set]] has an ALGORITHM behind it, and the algorithm has
// three parts that a program can tell apart.
//
// 1. THE VALUE IS VALIDATED. ToUint32 and ToNumber of the value must have the
//    same mathematical value, or it was never a length: a negative, a
//    fraction, NaN, a string that does not parse and 2^32 are all the
//    RangeError 10.4.2.4 step 3 names. A string that DOES parse is a length,
//    because ToUint32 is a conversion and not a type test.
// 2. SHRINKING DELETES. The elements above the new length stop being own
//    properties — `2 in a` goes false, `Object.keys` loses them, a callback
//    method stops visiting them — and growing back does NOT bring them back:
//    what the new slots hold is HOLES, because nothing between the old and the
//    new length is an own property. That is the difference between
//    `a.length = 0` and a fresh array only in that `a` keeps its identity, and
//    it is why `a.length = 0` is the idiom for clearing one.
// 3. THE OTHER STORAGE IS UNTOUCHED. A named property survives any length
//    change, and a named write never moves `length` — `a.foo = 1` on a
//    three-element array leaves it at 3, which is the whole reason `length` is
//    checked before the named path and not after it.
//
// And the integrity levels, from 7.3.14: `Object.freeze` makes `length`
// non-writable, so a change is refused (silently, in sloppy code) while a write
// of the SAME value still succeeds — 10.1.6.3 step 4 compares the values.
// `Object.seal` leaves `length` writable and makes the ELEMENTS
// non-configurable, so growing works and shrinking stops at the highest element
// it cannot delete, which for a full array means it does not move at all.
const a = [1, 2, 3, 4];
a.length = 2;
console.log(a.length, JSON.stringify(a));
console.log(a[2], 2 in a, a.hasOwnProperty(2));
console.log(Object.keys(a).join(","));

a.length = 4;
console.log(a.length, JSON.stringify(a));
console.log(a[2], 2 in a, a.hasOwnProperty(2));
console.log(Object.keys(a).join(","));
console.log(a);
let visits = 0;
a.forEach(() => {
  visits = visits + 1;
});
console.log(visits);
let ks = "";
for (const k in a) {
  ks = ks + k + "|";
}
console.log(ks);

a.length = 0;
console.log(a.length, JSON.stringify(a), a[0], Object.keys(a).length);

// Truncate, then push: the array is dense again from where it was cut.
const b = [1, 2, 3];
b.length = 1;
b.push(9);
console.log(JSON.stringify(b), b.length);

// A named property is not an element and survives every length change; and a
// named write does not move `length`.
const c = [1, 2, 3];
c.tag = "t";
console.log(c.length);
c.length = 0;
console.log(c.length, c.tag, Object.keys(c).join(","));

// ToUint32 accepts anything that round-trips, and refuses everything else.
const d = [1, 2, 3];
d.length = "2";
console.log(d.length);
d.length = 3.0;
console.log(d.length);

function setLength(v) {
  const x = [1, 2];
  try {
    x.length = v;
    return "set:" + x.length;
  } catch (e) {
    return e instanceof RangeError ? "RangeError" : "other";
  }
}
console.log(setLength(-1));
console.log(setLength(1.5));
console.log(setLength("abc"));
console.log(setLength(NaN));
console.log(setLength(4294967296));
console.log(setLength(true));
console.log(setLength("1"));

// A frozen array refuses a CHANGE and accepts a write of the same value.
const f = [1, 2, 3];
Object.freeze(f);
f.length = 0;
console.log(f.length, JSON.stringify(f));
f.length = 3;
console.log(f.length);

// A sealed array keeps a writable `length`: growing works, and shrinking stops
// at the highest element it may not delete.
const g = [1, 2, 3];
Object.seal(g);
g.length = 1;
console.log(g.length, JSON.stringify(g));
g.length = 5;
console.log(g.length, JSON.stringify(g), 4 in g);

// `a["length"]` and `a.length` are one property, so the computed spelling takes
// the same algorithm.
const h = [1, 2, 3];
h["length"] = 1;
console.log(h.length, JSON.stringify(h));

// `length` is non-configurable from birth (10.4.2.2), so deleting it fails —
// and fails without moving it.
const k = [1, 2];
console.log(delete k.length, k.length);
