// Array.prototype.splice (ECMA-262 23.1.3.31): deletion, insertion, growth,
// shrink, the removed-elements answer, and the integrity refusal.
//
// Derived from ECMA-262:
//
// 1. Steps 9-12 build the removed array from the deleted range — holes stay
//    holes, because CreateDataProperty runs only under a HasProperty test —
//    and the receiver closes the gap.
// 2. A negative start counts from the end (step 3) and the delete count is
//    clamped to what is there (step 7): splice(-2, 99) removes the last two.
// 3. No arguments at all deletes nothing (steps 5); ONE argument deletes to
//    the end (step 6); an `undefined` second argument is
//    ToIntegerOrInfinity(undefined) = 0, so splice(1, undefined) removes
//    nothing — the case that separates "absent" from "undefined".
// 4. Insertion without deletion shifts the tail up; more items than deletions
//    grows the array; the returned array is always the removed elements.
// 5. On a frozen array the shift's first Set is refused (10.1.9.2 answers
//    false for a non-writable property, and 23.1.3.31's Set throws on false),
//    so the TypeError arrives with the array unchanged.
const a = [1, 2, 3, 4, 5];
const removed = a.splice(1, 2);
console.log(removed.join(','));
console.log(a.join(','));
a.splice(1, 0, 9, 8);
console.log(a.join(','));
const r2 = a.splice(2, 1, 7);
console.log(r2.join(','), a.join(','));
const neg = [1, 2, 3, 4];
console.log(neg.splice(-2, 99).join(','), neg.join(','));
const b = [1, 2];
console.log(b.splice().length, b.length);
const c = [1, 2, 3];
const r3 = c.splice(1);
console.log(r3.join(','), c.length, c.join(','));
const d = [1, 2, 3];
console.log(d.splice(1, undefined).length, d.join(','));
const e = [1, 2, 3];
delete e[1];
const r4 = e.splice(0, 3);
console.log(r4.length, 0 in r4, 1 in r4, 2 in r4);
console.log(e.length);
const f = [1, 4];
f.splice(1, 0, 2, 3);
console.log(f.join(','));
const g = Object.freeze([1, 2, 3]);
try {
  g.splice(0, 1);
} catch (err) {
  console.log(err instanceof TypeError);
}
console.log(g.join(','));
