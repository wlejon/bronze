// A property read through a WARM inline-cache site must agree with the same
// read at a cold one, however the prototype chain was changed in between.
// ECMA-262 10.1.8.1 OrdinaryGet walks to the FIRST holder, so every line
// below has one right answer and the cache is not allowed to have a second.
//
// An entry is `(shape, slot, depth, epoch)` and a hit is taken on the
// RECEIVER's shape. At depth 0 and depth 1 that shape is the whole story: an
// own property that shadows changes it, and at depth 1 there is nothing
// between the receiver and the holder to shadow from. At depth 2 or more it
// is not, and the three ways a deeper chain can move are covered by three
// different mechanisms — which is why they are pinned together here:
//
// 1. An ADD to an intermediate prototype takes a shape transition and leaves no
// dictionary behind, so nothing on the walk looks different. The fill epoch is
// what catches it: any property add anywhere bumps a global counter, and a
// depth > 0 entry that does not match the current one is refused. `read(leaf)`
// must move from `top` to `mid`. 2. A DELETE renumbers slots, and a
// `setPrototypeOf` replaces the holder. Both put the object they touch into
// dictionary mode, and a dictionary anywhere on the cached walk is what
// `ObjectHeader::cachedProtoHolder` refuses. 3. An OWN add or delete changes
// the receiver's own shape word,
// which is the case the cache was designed for.
//
// Line ordering is load-bearing. Shadowing at the NEAREST prototype (line 5
// of the output) once passed only because the delete before it had left a
// dictionary on the walk; it is kept in that position so the case still
// covers the depth-1 add, and the epoch is what answers it now.
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
