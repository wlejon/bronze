// The integrity levels — `Object.freeze`, `Object.seal`,
// `Object.preventExtensions` and the three predicates — on every receiver kind
// that HAS one. ECMA-262 20.1.2.6/7/16/19/20/22 over 7.3.14 SetIntegrityLevel
// and 7.3.15 TestIntegrityLevel.
//
// The three operations are one algorithm with two dials, and the file is
// arranged around them: [[Extensible]] goes false in all three cases,
// `configurable` goes false for seal and freeze, and `writable` goes false for
// freeze alone. Everything below is that table read once per receiver kind.
//
// Why the kinds are worth a case each. 7.3.14 step 3 walks the object's OWN
// PROPERTY KEYS, and each kind keeps those somewhere else — a plain object in
// its shape and slots, an array in its element block plus `length`, a function
// in a side object plus `prototype`. A `freeze` written for one of them and not
// the others does not fail loudly; it succeeds and changes nothing, which is
// what makes `Object.isFrozen` reporting true about it the worst part rather
// than an incidental one.
//
// What each block pins:
//
//  1. The plain object, as the control. `cases/property_descriptors` and
//     `cases/object_prototype_statics` own it; it is here so that a change to
//     the shared algorithm shows up beside the kinds it is being extended to.
//  2. A frozen ARRAY refuses both a write to an existing element and a write
//     that would append. The second is not the first: appending is a CREATE and
//     is stopped by [[Extensible]] (10.1.6.3 step 2.b), which is why a merely
//     non-extensible array refuses it too.
//  3. A SEALED array still takes writes, which is the whole difference between
//     seal and freeze, and refuses `delete` — the operator's `false`, which is
//     the one refusal in this family a sloppy program can see as a value.
//  4. `preventExtensions` alone leaves the elements both writable AND
//     configurable, so a delete succeeds — and then the hole cannot be written
//     back, because re-creating an index needs the extensibility that was just
//     taken away. Once every index is gone the array is vacuously SEALED: its
//     only remaining own property is `length`, which 10.4.2 ArrayCreate makes
//     non-configurable from birth.
//  5. `length` is also what makes an EMPTY non-extensible array sealed but not
//     frozen: it stays writable until `freeze`, and 7.3.15 checks writability
//     for the frozen question only.
//  6. A FUNCTION's statics are its own properties, so freezing one has to reach
//     the side object they live in. A `static` method is one of them.
//  7. `prototype` is the function's `length`: non-configurable but writable
//     (10.2.4), so `freeze` is the only thing that stops it being reassigned —
//     and it lives in a slot of its own, which is why nothing in the statics
//     table can answer for it.
//  8. A PRIMITIVE. 20.1.2.6 step 1 returns it unchanged rather than throwing,
//     and 7.3.15 step 1 calls it frozen and sealed vacuously — while
//     `isExtensible` answers false for it (20.1.2.16 step 1), the one member of
//     the six whose primitive answer is not the agreeable one.
//
// A Map, a Set, a typed array and a RegExp are NOT here. bronze keeps no
// property table for them and so has nowhere to record [[Extensible]]; every
// operation that would is a hard error naming the kind, and the predicates
// answer `extensible: true` — which is correct precisely because the only route
// to false is refused loudly. The typed array's own answer, which the language
// specifies rather than bronze conceding, is `cases/integrity_typed_array`.

// 1. the plain object
const o = { a: 1 };
console.log(Object.isExtensible(o), Object.isSealed(o), Object.isFrozen(o));
Object.freeze(o);
console.log(Object.isExtensible(o), Object.isSealed(o), Object.isFrozen(o));

// 2. a frozen array
const a = [1, 2];
console.log(Object.isExtensible(a), Object.isSealed(a), Object.isFrozen(a));
console.log(Object.freeze(a) === a);
a[0] = 9;
a[2] = 3;
console.log(a[0], a[2], a.length);
console.log(Object.isExtensible(a), Object.isSealed(a), Object.isFrozen(a));
console.log(delete a[0], a[0]);

// 3. a sealed array
const s = Object.seal([1, 2]);
s[0] = 9;
s[2] = 3;
console.log(s[0], s[2], s.length);
console.log(delete s[1], s[1]);
console.log(Object.isExtensible(s), Object.isSealed(s), Object.isFrozen(s));

// 4. preventExtensions alone
const p = Object.preventExtensions([1, 2]);
p[0] = 9;
console.log(p[0], p.length);
console.log(delete p[1], p[1], p.length);
p[1] = 5;
console.log(p[1]);
console.log(Object.isExtensible(p), Object.isSealed(p), Object.isFrozen(p));
console.log(delete p[0], Object.isSealed(p), Object.isFrozen(p));

// 5. the empty array, where `length` is the only own property left
console.log(Object.isSealed(Object.preventExtensions([])));
console.log(Object.isFrozen(Object.preventExtensions([])));
console.log(Object.isFrozen(Object.freeze([])));

// 6. a function and its statics
function fn() {}
fn.tag = "t";
console.log(Object.isExtensible(fn), Object.isSealed(fn), Object.isFrozen(fn));
console.log(Object.freeze(fn) === fn);
fn.tag = "changed";
fn.other = 1;
console.log(fn.tag, fn.other);
console.log(Object.isExtensible(fn), Object.isSealed(fn), Object.isFrozen(fn));
console.log(delete fn.tag, fn.tag);

function sf() {}
sf.n = 1;
Object.seal(sf);
sf.n = 2;
sf.m = 3;
console.log(sf.n, sf.m, delete sf.n);
console.log(Object.isSealed(sf), Object.isFrozen(sf));

class C {
  static make() {
    return "made";
  }
}
Object.freeze(C);
C.make = function () {
  return "replaced";
};
console.log(C.make());

// 7. `prototype`
function pf() {}
const before = pf.prototype;
Object.freeze(pf);
pf.prototype = { replaced: true };
console.log(pf.prototype === before);

// 8. primitives
console.log(Object.freeze(5), Object.isFrozen(5), Object.isSealed("s"), Object.isExtensible(5));
console.log(Object.seal(null), Object.preventExtensions(undefined));
