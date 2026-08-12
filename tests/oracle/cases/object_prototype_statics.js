// The rest of the `Object` namespace — `create`, `seal`, `isSealed`,
// `preventExtensions`, `isExtensible`, `getPrototypeOf`, `setPrototypeOf`,
// `getOwnPropertyNames` and `defineProperties` (ECMA-262 20.1.2), promoted
// from the named errors they used to be.
//
// Two of them are more than plumbing:
//
//  - `Object.create(null)` makes an object with NO prototype, which every walk
//    over a prototype chain has to tolerate. It needs no special case in
//    bronze because the prototype lives on the shape and a shape whose
// prototype is not an object already ends the walk.
//  - `Object.setPrototypeOf` moves a link an inline cache may have walked
//    THROUGH. An entry is `(shape, slot, depth)` and the receiver's shape is
//    all it checks, so a swap two links up is invisible to it — which is the
//    same hole `cases/blocked/proto_chain_invalidation.js` pins for an ADD.
//    The swap is made safe by putting the object it touches into dictionary
//    mode, which every cached proto-walk now refuses to cross; the last two
//    blocks below are that, read through a warm site and a cold one.
const proto = { greet() { return "hi " + this.name; }, kind: "base" };
const made = Object.create(proto);
made.name = "a";
console.log(made.greet(), made.kind);
console.log(Object.keys(made).join(","), Object.getPrototypeOf(made) === proto);

const bare = Object.create(null);
bare.x = 1;
console.log(bare.x, Object.getPrototypeOf(bare), "x" in bare);

// 20.1.2.6 SetIntegrityLevel(O, sealed): configurable off, extensible off,
// and writable LEFT ALONE — which is the whole difference from freeze.
const sealed = Object.seal({ a: 1, b: 2 });
sealed.a = 9;
delete sealed.b;
sealed.c = 3;
console.log(sealed.a, sealed.b, sealed.c);
console.log(Object.isSealed(sealed), Object.isFrozen(sealed), Object.isExtensible(sealed));

// preventExtensions is weaker still: nothing may be ADDED, and everything
// already there stays writable and configurable. Deleting the last property
// then makes the object vacuously sealed, which is 7.3.15 read literally.
const open = { d: 1 };
console.log(Object.isExtensible(open), Object.isSealed(open), Object.isFrozen(open));
Object.preventExtensions(open);
open.e = 2;
open.d = 5;
console.log(open.d, open.e, Object.isExtensible(open));
delete open.d;
console.log(Object.isSealed(open), Object.keys(open).length);

const desc = Object.defineProperties({}, {
  visible: { value: 1, enumerable: true },
  hidden: { value: 2 },
});
console.log(desc.visible, desc.hidden);
console.log(Object.keys(desc).join(","), Object.getOwnPropertyNames(desc).join(","));

const created = Object.create(proto, { own: { value: 9, enumerable: true } });
console.log(created.own, created.kind, Object.keys(created).join(","));

const one = { tag: "one" };
const two = { tag: "two" };
const swapped = { v: 1 };
function readTag(o) { return o.tag; }
Object.setPrototypeOf(swapped, one);
console.log(readTag(swapped), readTag(swapped));
Object.setPrototypeOf(swapped, two);
console.log(readTag(swapped), swapped.tag);
console.log(Object.getPrototypeOf(swapped) === two, Object.keys(swapped).join(","));

const top1 = { p: "top1" };
const top2 = { p: "top2" };
const mid = Object.create(top1);
const leaf = Object.create(mid);
function readP(o) { return o.p; }
console.log(readP(leaf), readP(leaf));
Object.setPrototypeOf(mid, top2);
console.log(readP(leaf), leaf.p);

const cut = Object.create(one);
console.log(cut.tag);
Object.setPrototypeOf(cut, null);
console.log(cut.tag, Object.getPrototypeOf(cut));
