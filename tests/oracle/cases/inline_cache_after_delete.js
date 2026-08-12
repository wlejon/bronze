// The two ways a warm inline cache can be told a lie by a delete, and the
// checks that stop it. Both are silent wrong answers if they get through:
// the program reads a real value out of a real slot, just not the one it
// asked for.
//
// 1. THE RECEIVER'S OWN PROPERTY IS DELETED. The entry says {shape S, depth 0,
// slot k}. A delete moves the object to dictionary mode, which gives it a
// private shape, so the shape word no longer equals S and the entry cannot hit
// — and the read falls through to the prototype's copy, which the own property
// was shadowing. Writing `undefined` over the property could never do that.
//
// 2. THE PROTOTYPE IS DELETED FROM. This is the sharp one. The entry says
//    {shape S, depth 1, slot k}, and S is the RECEIVER'S shape — which does
//    not change when its prototype is edited. So the shape compare still
//    matches, and the cached slot is read off an object whose slot numbering
//    a delete has since rearranged: the freed slot goes to the next property
//    added, and the read returns THAT property's value under the old name.
//    A cached proto hit re-checks the holder for exactly this.
//
// EVERY READ BELOW GOES THROUGH ONE FUNCTION, on purpose. Two `x.b`
// expressions on two source lines are two sites with two cache entries, both
// cold, and neither would ever be handed the stale answer this case is
// written to catch. One function is one site, warmed by the reads before the
// delete and re-entered after it.
//
// Derived from ECMA-262 10.5.6 ([[Delete]] removes an own property and says
// nothing about the prototype), 10.1.8.1 (OrdinaryGet walks the chain on an
// own miss), 7.3.12 (`in` walks it too) and 13.5.1 (the boolean result).

function readV(x) {
  return x.v;
}
function readB(x) {
  return x.b;
}
function readA(x) {
  return x.a;
}

// ---- 1. an own property, shadowing an inherited one ------------------------
function Holder() {
  this.v = "own";
}
Holder.prototype.v = "proto";

const h = new Holder();
console.log(readV(h));
console.log(readV(h));
console.log(delete h.v);
console.log(readV(h));
console.log("v" in h);
console.log(Object.keys(h).length);

// Re-adding creates a NEW own property, which shadows again.
h.v = "own2";
console.log(readV(h));

// ---- 2. a prototype whose slots get rearranged -----------------------------
function P() {}
P.prototype.a = "pa";
P.prototype.b = "pb";

const q = new P();
// Warm at depth 1: `b` is not an own property of q, so the entry records how
// far up the chain it was found, and the receiver's shape is all it checks.
console.log(readB(q));
console.log(readB(q));

// The prototype becomes a dictionary, and `b`'s slot goes on its free list.
console.log(delete P.prototype.b);
// A new property takes that freed slot. A cache that still trusted {q's
// shape, depth 1, b's old slot} answers "pz" on the next line.
P.prototype.z = "pz";
console.log(readB(q));
console.log(readA(q));
console.log("b" in q);

// And the same slot again, under the name the cache remembers: `a` is freed
// and `b` re-added into it, so a stale entry would now answer "pb2" for `a`.
console.log(delete P.prototype.a);
P.prototype.b = "pb2";
console.log(readA(q));
console.log(readB(q));
