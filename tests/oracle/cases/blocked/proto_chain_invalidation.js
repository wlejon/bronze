// BLOCKED, and unlike the rest of this directory it is blocked on a BUG, not
// on a missing feature: bronze compiles and runs every line below and prints
// the wrong answer for two of them, identically with inference and with
// `--no-infer`. It is here rather than in `cases/` because promoting it would
// mean pinning the wrong bytes, and the rule is that an expectation is
// derived from ECMA-262 and never from what bronze does (docs/0003).
//
// The bug is in the inline caches of docs/0010 decision 7, and it is the one
// hole docs/0019 decision 5 does NOT close. A cache entry is
// `(shape, slot, depth)`, and a hit is taken when the RECEIVER's shape word
// matches. At depth 0 and depth 1 that is sound: an own property shadowing
// the cached one changes the receiver's shape and misses, and at depth 1
// there is nothing between the receiver and the holder to shadow from. At
// depth 2 or more it is not. Adding `p` to an INTERMEDIATE prototype changes
// only that prototype's shape — the receiver's is untouched — so the entry
// still hits, still walks `depth` links, and still reads the property it
// shadowed. `read(leaf)` answers `"top"` where `leaf.p` at a cold site
// answers `"mid"`, which is the signature of the bug: two reads of the same
// property in the same program disagree.
//
// The DELETE half of the same question is already closed. A delete puts the
// object into dictionary mode, and a dictionary anywhere on the cached walk is
// what `ObjectHeader::cachedProtoHolder` refuses (docs/0019 decision 5,
// widened to the intermediate links by docs/0022) — so lines 4 and 5 below are
// right today, and so is every `Object.setPrototypeOf`, which takes the same
// escape deliberately. It is only the ADD that gets through, because an add is
// exactly the operation the shape chain is designed to make free.
//
// The fix is a prototype-validity check, and it needs more than the 16 bytes
// an entry has: something has to be able to say "some object between this
// receiver and this holder has changed since you filled this". The two shapes
// it could take are a per-prototype-object validity cell that the entry
// points at (V8's), or a global epoch that any add-to-a-prototype bumps and
// that every depth>0 entry records. Either one widens `InlineCache`, which
// widens the stride that `src/codegen-llvm/llvm_prop.cpp` inlines, so it is a
// change to the generated-code contract and not a runtime-only repair — which
// is why it is its own chunk rather than a patch on the end of docs/0019's.
//
// What this case pins when it lands, from ECMA-262 10.1.8.1 (OrdinaryGet
// walks to the FIRST holder) and 10.1.9.2:
//
// 1. A three-link chain read through one warm site, then shadowed at the
//    MIDDLE link: the read must move to the middle one. (Wrong today: `top`.)
// 2. The same property read at a second, cold site must agree with the first.
//    (Right today, and that disagreement is how the bug shows.)
// 3. Deleting the shadow must move the read back up. (Right today, for the
//    dictionary reason above.)
// 4. Shadowing at the NEAREST prototype instead — still not the receiver, so
//    still no change to the receiver's shape. (Right today, but only by
//    accident: the delete on the line before left a dictionary on the walk,
//    which is what makes the entry miss. Reorder the case and it breaks.)
// 5. An own property shadows everything, and deleting it unshadows back to
//    the nearest prototype. (Right today: an own add and an own delete both
//    change the receiver's own shape word, which is the case the cache was
//    designed for.)
class TopC {}
TopC.prototype.p = "top";
class MidC extends TopC {}
class LeafC extends MidC {}
const leaf = new LeafC();
function read(o) {
  return o.p;
}
console.log(read(leaf));
MidC.prototype.p = "mid";
console.log(read(leaf));
console.log(leaf.p);
delete MidC.prototype.p;
console.log(read(leaf));
LeafC.prototype.p = "leaf";
console.log(read(leaf));
leaf.p = "own";
console.log(read(leaf));
delete leaf.p;
console.log(read(leaf));
