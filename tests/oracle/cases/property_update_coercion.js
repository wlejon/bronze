// The two halves of `o.k++` that `property_update_operators` does not reach:
// what ToNumeric does to the old value, and how many times the reference is
// evaluated to produce it (ECMA-262 13.4.4.1 / 13.4.5.1).
//
// An update is NOT `o.k = o.k + 1`. Step 2 of each of 13.4.{2,3,4,5}.1 is
// ToNumeric(GetValue(ref)) — the coercion happens BEFORE the arithmetic and
// before the write — so `o.s = "5"; o.s++` leaves the number 6 where `+` on
// the same operands would have concatenated, and the expression's value is
// the number 5 and not the string. And the reference is evaluated once for
// both the read and the write, so a base with a side effect has it once.
//
// What this pins:
//
// 1. ToNumeric on each primitive the property may hold: a numeric string
//    parses (7.1.4.1), a non-numeric one is NaN, `undefined` is NaN, `true`
//    is 1 and `null` is +0. The value left behind is a Number in every case,
//    which `typeof` confirms.
// 2. The base expression, the computed key expression, and the index of
//    `a[i]++` are each evaluated exactly ONCE. `a[i++]++` is the shape that
//    catches a second evaluation: `i` would otherwise advance twice.
// 3. An accessor pair is read through the getter and written through the
//    setter, both with the RECEIVER as `this` (docs/0019 decision 4) — so a
//    setter that transforms its argument is visible in the next read, and an
//    accessor inherited from a prototype updates the instance it ran on and
//    not the prototype.
// 4. A dictionary-mode object (one a `delete` has taken out of the shape
//    chain, docs/0019 decision 1) updates like any other.
// 5. An element of an array is a reference like any other, and `a.length`
//    does not move when an existing one is updated.

const o = { s: "5", u: undefined, n: NaN, bad: "xyz", t: true, z: null };
console.log(o.s++, o.s);
console.log(++o.u, o.u);
console.log(o.n++, o.n);
console.log(o.bad--, o.bad);
console.log(o.t++, o.t);
console.log(o.z++, o.z);
console.log(typeof o.s, typeof o.t, typeof o.bad);

// The base is evaluated once: `base()` counts its own calls.
let calls = 0;
function base() {
  calls++;
  return o;
}
base().s++;
console.log(calls, o.s);

// The computed key is evaluated once too, and its value is used for both the
// read and the write — a second call would also be a second `keyCalls`.
let keyCalls = 0;
function key() {
  keyCalls++;
  return "s";
}
o[key()]++;
console.log(keyCalls, o.s);

// `a[i++]++`: the index expression advances `i` once, and it is the value it
// produced — not the one after — that both the read and the write use.
const a = [1, 2, 3];
let i = 0;
a[i++]++;
console.log(a[0], a[1], i);

// An accessor pair, read and written through the same receiver.
const acc = {
  _v: 1,
  get v() {
    return this._v;
  },
  set v(x) {
    this._v = x * 100;
  }
};
console.log(acc.v++, acc._v, acc.v);

// The accessor lives on the prototype; the update must land on the instance
// that ran it, so two instances do not share a count.
class Counter {
  constructor() {
    this._n = 0;
  }
  get n() {
    return this._n;
  }
  set n(v) {
    this._n = v;
  }
  bump() {
    return this.n++;
  }
}
const c1 = new Counter();
const c2 = new Counter();
c1.bump();
c1.bump();
console.log(c1.bump(), c1.n, c2.n);

// Dictionary mode: `delete` takes the object out of the shape chain, and the
// update path must not care.
const d = { p: 1, q: 2, r: 3 };
delete d.q;
d.p++;
++d.r;
console.log(d.p, d.r);

// An array element, updated in place: `length` is unchanged, and the element
// that was not touched is unchanged with it.
const arr = [0, 1, 2];
arr[2]++;
--arr[0];
console.log(arr.join(","), arr.length);

// A chain of members: only the LAST link is the reference being updated.
const outer = { inner: { k: 1 } };
outer.inner.k++;
console.log(outer.inner.k);
