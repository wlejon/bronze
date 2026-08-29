// The attributes of an own property, across the conversion that gives an
// object a private property table.
//
// ECMA-262 6.2.6.1 gives a data property four attributes, and 10.1.6.3
// ValidateAndApplyPropertyDescriptor is the only thing allowed to change one.
// Nothing in the language lets an object QUIETLY regain `writable` or
// `configurable`: a property defined `writable: false` is non-writable until a
// [[DefineOwnProperty]] with a configurable current descriptor says otherwise.
//
// That is worth pinning because bronze keeps those attributes in two places.
// A property normally lives in a shared shape node, which carries all four; an
// object that needs a per-object answer — one a shared node cannot give, which
// `delete` and `Object.preventExtensions` both force — moves its properties
// into a private table instead. The move is invisible to the language and must
// stay that way, and the way it fails is the worst kind: `preventExtensions`
// and `freeze` differ in that `freeze` re-stamps every attribute afterwards
// and `preventExtensions` does not, so a conversion that dropped attributes
// was hidden behind `freeze` and reachable through `preventExtensions`.
//
// What each line pins:
//
// 1. The descriptor of a non-writable, non-configurable property, before
//    anything can have converted the object. This is the baseline the rest
//    must equal.
// 2. The same descriptor after `Object.preventExtensions`, which sets
//    [[Extensible]] false and — 20.1.2.19 through 10.1.4.1 — touches no
//    property attribute at all.
// 3. The same descriptor after a `delete` of an UNRELATED key. 13.5.1.2
//    removes the deleted property and says nothing about any other.
// 4. A sloppy write to the property is discarded and a strict one is a
//    TypeError (10.1.9.2 step 2.a, then 13.15.2 PutValue step 6.d) — in both
//    converted objects, because a non-writable property is non-writable
//    however the object stores it.
// 5. `delete` of the non-configurable property answers false in sloppy code
//    (13.5.1.2 step 5.b) and is a TypeError in strict code (step 5.a.iii),
//    again in both.
// 6. A property that is merely non-CONFIGURABLE keeps its writability: the
//    write lands and only the delete refuses. The two attributes are separate,
//    and a conversion that restored one would be caught here even if it left
//    the other alone.
// 7. `enumerable` and `accessor` survive too — they always did, and they are
//    read back here so that "all four" is what the case pins rather than "the
//    two that were broken".

function desc(obj, key) {
  const d = Object.getOwnPropertyDescriptor(obj, key);
  return [d.writable, d.enumerable, d.configurable, d.value].join(",");
}

function locked() {
  const o = {};
  Object.defineProperty(o, "x", {
    value: 1,
    writable: false,
    enumerable: true,
    configurable: false,
  });
  return o;
}

const fresh = locked();
console.log(desc(fresh, "x"));

const closed = locked();
Object.preventExtensions(closed);
console.log(desc(closed, "x"));

const deleted = locked();
deleted.spare = 2;
delete deleted.spare;
console.log(desc(deleted, "x"));

// Sloppy: both refusals are silent, and neither changes anything.
closed.x = 99;
deleted.x = 99;
console.log(closed.x, deleted.x);
console.log(delete closed.x, delete deleted.x);
console.log(closed.x, deleted.x);

function strictly() {
  "use strict";
  try {
    closed.x = 99;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    deleted.x = 99;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    delete closed.x;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    delete deleted.x;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
}
strictly();
console.log(closed.x, deleted.x);

// Non-configurable but WRITABLE: the write lands, the delete refuses.
const halfLocked = {};
Object.defineProperty(halfLocked, "y", {
  value: 1,
  writable: true,
  enumerable: true,
  configurable: false,
});
halfLocked.spare = 2;
delete halfLocked.spare;
console.log(desc(halfLocked, "y"));
halfLocked.y = 7;
console.log(halfLocked.y, delete halfLocked.y, halfLocked.y);

// Non-enumerable, and an accessor, across the same conversion.
const hidden = {};
Object.defineProperty(hidden, "z", {
  value: 1,
  writable: false,
  enumerable: false,
  configurable: false,
});
Object.defineProperty(hidden, "g", {
  get() {
    return 5;
  },
  enumerable: false,
  configurable: false,
});
hidden.spare = 2;
delete hidden.spare;
console.log(desc(hidden, "z"));
console.log(Object.keys(hidden).length, hidden.g);
const gd = Object.getOwnPropertyDescriptor(hidden, "g");
console.log(typeof gd.get, gd.set, gd.enumerable, gd.configurable);
