// NAMED OWN-SLOT READS STANDING INSIDE A RUN OF ELEMENT ACCESSES.
//
// A `o.name` read whose class layout proved a constant instance slot compiles to
// a shape question and a load at a compile-time offset
// (src/codegen-llvm/llvm_static_slot.h). A stretch of element accesses off one
// receiver is proven once and spent per member
// (src/codegen-llvm/llvm_recv_proof.h). llvm_run_arms.h puts the two together:
// the shape question is hoisted to the head of a SPAN and the read becomes a
// step of it — a GEP and a load with no branch of its own — so that a named read
// standing between two element accesses stops cutting the run in half.
// `Matrix4.compose` is the shape it exists for: twelve constant-index stores
// into `this.elements`, then `te[12] = position.x` three times.
//
// Everything the hoisted question is allowed to skip is a thing the RUNTIME
// checked before it published the site's cell or stamped the shape — an object,
// a plain one, not a dictionary, the key an OWN DATA property, at exactly the
// claimed slot (runtime/static_shape.cpp, runtime/class_family.cpp). So the
// cases below come in three groups.
//
// THE SITE WITH NO CLAIM, whose receiver's class is not one thing: a plain
// literal with the three names and with the three names reordered, an accessor,
// a Proxy, a prototype hit. Those pin what the answer IS, whatever compiles it.
//
// THE IDENTITY SITE, whose receiver is exact by construction, so each read holds
// a module cell pinned to one shape.
//
// THE FAMILY SITE — `this` inside a method of a class somebody extends, which is
// three.js's `Object3D.updateMatrixWorld` — where the guard asks the shape's
// stamp about a subtree of classes instead of pinning one shape.
//
// Both claimed sites are then fed instances whose STATIC class never changed and
// on which one of the runtime's conditions has stopped holding: the slot
// redefined into a getter, a property added so the shape transitions, and a
// delete that drops the object into dictionary mode. Those are the ones the
// hoisted question has to keep missing on.
//
// This file is a Script. Nothing in it depends on strictness.

function seq(n) {
  const a = [];
  for (let i = 0; i < n; i++) a.push(0);
  return a;
}

function show(a) {
  let s = '';
  for (let i = 0; i < a.length; i++) {
    if (i > 0) s += ',';
    s += String(a[i]);
  }
  return s;
}

class Vec3 {
  constructor(x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
  }

  // The FAMILY site: `this` in a method of a class Vec4 extends.
  spread(out) {
    out[0] = 7;
    out[1] = 8;
    out[2] = this.x;
    out[3] = this.y;
    out[4] = this.z;
    return out;
  }
}

class Vec4 extends Vec3 {
  constructor(x, y, z, w) {
    super(x, y, z);
    this.w = w;
  }
}

// ---- the site with no claim ------------------------------------------------

function pack(out, v) {
  out[0] = 1;
  out[1] = 2;
  out[2] = 3;
  out[3] = v.x;
  out[4] = v.y;
  out[5] = v.z;
  return out;
}

// 1. A plain object literal with the three names, and then the same three names
//    in the reverse order. 10.1.8.1 answers by NAME; where a name sits is the
//    implementation's business and never the program's.
console.log('literal|' + show(pack(seq(6), { x: 7, y: 8, z: 9 })));
console.log('reordered|' + show(pack(seq(6), { z: 30, y: 20, x: 10 })));

// 2. An ACCESSOR at one of the keys. 10.4.1's [[Get]] calls it with the receiver
//    as `this`, so the answer depends on a property read of its own.
function accessorAtTheSite() {
  const o = {
    x: 1,
    get y() { return this.x * 100; },
    z: 3
  };
  return show(pack(seq(6), o));
}

console.log('accessor|' + accessorAtTheSite());

// 3. A PROXY receiver: the trap runs, once per read, in source order.
function proxyAtTheSite() {
  const log = [];
  const p = new Proxy({ x: 1, y: 2, z: 3 }, {
    get: function (t, k) {
      log.push(String(k));
      return t[k] * 11;
    }
  });
  return show(pack(seq(6), p)) + '|' + log.join('.');
}

console.log('proxy|' + proxyAtTheSite());

// 4. One key on a PROTOTYPE, where no slot of the receiver can hold it
//    (10.1.8.1 step 3 walks), mixed with two own ones.
function protoRead() {
  const base = { y: 200, w: 42 };
  const o = Object.create(base);
  o.x = 1;
  o.z = 3;
  return show(pack(seq(6), o)) + '|' + o.w;
}

console.log('proto|' + protoRead());

// ---- the identity site -----------------------------------------------------
//
// Every argument below is a `new Vec3(...)`, so the receiver's class is exact by
// construction and each read pins one shape in a cell of its own.

function packVec(out, v) {
  out[0] = 1;
  out[1] = 2;
  out[2] = 3;
  out[3] = v.x;
  out[4] = v.y;
  out[5] = v.z;
  return out;
}

// 5. The plain shape.
console.log('pack|' + show(packVec(seq(6), new Vec3(10, 20, 30))));

// 6. A property ADDED to an instance, so its shape is no longer the one the
//    constructor left. An add appends, so x, y and z keep the positions they
//    had, and the answers are the instance's own.
function addedField() {
  const v = new Vec3(1, 2, 3);
  v.extra = 5;
  return show(packVec(seq(6), v)) + '|' + v.extra;
}

console.log('added|' + addedField());

// 7. The slot REDEFINED into a getter, after a data-property instance has
//    already been through the site. 10.1.6.3 replaces the data property in
//    place, so the second call must call where the first loaded.
function redefinedBetweenCalls() {
  const v = new Vec3(1, 2, 3);
  const first = show(packVec(seq(6), v));
  Object.defineProperty(v, 'y', { get: function () { return 55; }, configurable: true });
  return first + '|' + show(packVec(seq(6), v));
}

console.log('redefined|' + redefinedBetweenCalls());

// 8. A DICTIONARY-MODE receiver. One delete of an own configurable property is
//    enough to take an object off the transition tree; the answers do not
//    change, because a dictionary is a different way to hold a property and not
//    a different property.
function dictionaryAtTheSite() {
  const v = new Vec3(1, 2, 3);
  v.tmp = 9;
  const gone = delete v.tmp;
  return show(packVec(seq(6), v)) + '|' + gone + '|' + (v.tmp === undefined);
}

console.log('dictionary|' + dictionaryAtTheSite());

// 9. The read THROWS in the middle of the span, from a getter installed over the
//    slot. The element stores in front of it stand and the ones behind it were
//    never made, which is the property a span emitted as two arms has to keep:
//    nothing may be stored on a path the program did not reach.
function throwsMidRun() {
  const out = seq(6);
  const v = new Vec3(1, 2, 3);
  Object.defineProperty(v, 'y', {
    get: function () { throw new RangeError('mid'); },
    configurable: true
  });
  let caught = '';
  try {
    packVec(out, v);
  } catch (e) {
    caught = e.name;
  }
  return caught + '|' + show(out);
}

console.log('threw|' + throwsMidRun());

// 10. A named store that ADDS a property BETWEEN two own-slot reads of the same
//     receiver, so the shape the second read meets is one the block in front of
//     it produced. The second call then arrives at a shape the first call's own
//     add already made.
function addBetweenReads(out, v) {
  out[0] = v.x;
  v.extra = 5;
  out[1] = v.y;
  out[2] = v.extra;
  return out;
}

function addedMidRun() {
  const a = show(addBetweenReads(seq(3), new Vec3(1, 2, 3)));
  const b = show(addBetweenReads(seq(3), new Vec3(4, 5, 6)));
  return a + '|' + b;
}

console.log('added-mid|' + addedMidRun());

// 11. Three fields of one object, read off a local whose class is exact: three
//     shape compares against three cells, off one header.
function identityForm() {
  const v = new Vec3(3, 4, 5);
  const out = seq(6);
  out[0] = 9;
  out[1] = v.x;
  out[2] = v.y;
  out[3] = v.z;
  out[4] = 9;
  return show(out);
}

console.log('identity|' + identityForm());

// ---- the family site -------------------------------------------------------
//
// 12. The base's own instance, then a SUBCLASS's. 15.7.14 runs the base
//     constructor to completion before the derived class installs a field of
//     its own, so Vec4's properties begin with Vec3's, in Vec3's order and at
//     Vec3's positions — which is the whole claim a family stamp stands for.
console.log('fam-base|' + show(new Vec3(1, 2, 3).spread(seq(5))));
console.log('fam-sub|' + show(new Vec4(4, 5, 6, 9).spread(seq(5))));

// 13. The same three conditions against the family guard: a shape the class's
//     constructor did not leave, an accessor over one of the fields, and a
//     dictionary. A stamp is only ever written for a shape whose prefix really
//     is the class's field list, so the last two must miss.
function familyAdded() {
  const v = new Vec3(1, 2, 3);
  v.extra = 5;
  return show(v.spread(seq(5)));
}

function familyRedefined() {
  const v = new Vec3(1, 2, 3);
  Object.defineProperty(v, 'y', { get: function () { return 55; }, configurable: true });
  return show(v.spread(seq(5)));
}

function familyDictionary() {
  const v = new Vec3(1, 2, 3);
  v.tmp = 9;
  delete v.tmp;
  return show(v.spread(seq(5)));
}

console.log('fam-added|' + familyAdded());
console.log('fam-redefined|' + familyRedefined());
console.log('fam-dictionary|' + familyDictionary());

// 14. Own-slot reads feeding a run of TYPED-ARRAY stores, which is
//     `Color.toArray`'s shape. The gate hoists the reads above the stores
//     between them, and that is legal because the reads' receiver was proven
//     PLAIN and the stores' a typed array, so no load here can see one of them.
function toF32(out, v, off) {
  out[off] = v.x;
  out[off + 1] = v.y;
  out[off + 2] = v.z;
  return out;
}

function typedRun() {
  const f = new Float32Array(6);
  toF32(f, new Vec3(0.5, 0.25, 0.125), 0);
  toF32(f, new Vec3(1.5, 2.25, 4.125), 3);
  let s = '';
  for (let i = 0; i < f.length; i++) {
    if (i > 0) s += ',';
    s += String(f[i]);
  }
  return s;
}

console.log('typed|' + typedRun());
