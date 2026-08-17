// The ES2025 SET OPERATIONS (ECMA-262 24.2.4): `union`, `intersection`,
// `difference`, `symmetricDifference`, `isSubsetOf`, `isSupersetOf` and
// `isDisjointFrom`.
//
// Derived from ECMA-262:
//
// 1. The four that build a set answer a NEW Set and leave both operands alone.
// 2. The ORDER of the result is this set's insertion order, with a `union`'s
//    new elements appended in the other side's order (24.2.4.17 step 4 starts
//    from a copy of this set's data), and a `symmetricDifference` likewise
//    (24.2.4.14 step 4).
// 3. The argument is not a Set but a SET-LIKE: 24.2.1.2 GetSetRecord reads
//    `size`, `has` and `keys` off it and uses nothing else. A Map is one (its
//    `keys` yields its keys), and so is a hand-written `{size, has, keys}`.
// 4. Which side gets WALKED depends on the two sizes — the smaller is iterated
//    and the larger is asked `has` (24.2.4.9 step 6 vs step 7) — but the ANSWER
//    does not, and neither does the result's order: step 7.b.iv puts the
//    result back into this set's relative order.
// 5. The predicates use `size` to answer without any call at all when the
//    counts make the answer impossible: 24.2.4.11 step 4 for `isSubsetOf` and
//    24.2.4.12 step 4 for `isSupersetOf`.
// 6. 7.2.11 CanonicalizeKeyedCollectionKey: a -0 taken from the other side is
//    stored as +0.
const a = new Set([1, 2, 3]);
const b = new Set([3, 4]);

// 1 & 2: the four builders.
console.log([...a.union(b)].join(','));
console.log([...a.intersection(b)].join(','));
console.log([...a.difference(b)].join(','));
console.log([...a.symmetricDifference(b)].join(','));
console.log([...a].join(','), [...b].join(','));
console.log(a.union(b) === a, a.union(b) === b, typeof a.union(b).add);
console.log(a.union(b).size, a.intersection(b).size, a.difference(b).size);

// Order: this set first, in its own order, then the other side's newcomers.
const order = new Set(['c', 'a', 'b']);
console.log([...order.union(new Set(['z', 'a', 'y']))].join(','));
console.log([...order.symmetricDifference(new Set(['a', 'q']))].join(','));

// 3: the predicates.
console.log(a.isSubsetOf(new Set([1, 2, 3, 4])), a.isSubsetOf(b));
console.log(a.isSupersetOf(new Set([1, 2])), a.isSupersetOf(b));
console.log(a.isDisjointFrom(new Set([9])), a.isDisjointFrom(b));
console.log(a.isSubsetOf(a), a.isSupersetOf(a), a.isDisjointFrom(a));
// An empty set is a subset of everything, a superset of nothing but itself,
// and disjoint from everything.
const empty = new Set();
console.log(empty.isSubsetOf(a), empty.isSupersetOf(a), empty.isDisjointFrom(a));
console.log(a.isSupersetOf(empty), a.isDisjointFrom(empty));

// 3: a Map as the set-like.
const m = new Map([[3, 'x'], [5, 'y']]);
console.log([...a.intersection(m)].join(','));
console.log([...a.union(m)].join(','));
console.log([...a.difference(m)].join(','));
console.log(a.isDisjointFrom(m), a.isSupersetOf(m));

// 3: a hand-written set-like, and the fact `has` is called on IT.
let hasCalls = 0;
const setLike = {
  size: 2,
  has(v) {
    hasCalls += 1;
    return v === 1 || v === 9;
  },
  keys() {
    return [1, 9][Symbol.iterator]();
  },
};
// `has` is called on the SET-LIKE, and only on the arm that walks this set:
// one element against a size-2 other takes 24.2.4.11 step 5.
console.log(new Set([1]).isSubsetOf(setLike), hasCalls);
console.log([...a.intersection(setLike)].join(','));
console.log([...a.union(setLike)].join(','));
console.log([...a.difference(setLike)].join(','));
console.log(a.isDisjointFrom(setLike), a.isSupersetOf(setLike), a.isSubsetOf(setLike));

// 4: the large-other arm — six elements against three, so the other side's
// `keys` is walked and the result is still in this set's order.
const big = new Set([30, 3, 20, 2, 10, 1]);
console.log([...a.intersection(big)].join(','));
console.log([...a.difference(new Set([3, 9, 8, 7, 6, 5]))].join(','));
console.log(a.isDisjointFrom(big), a.isSupersetOf(big));

// 6: -0 arrives as +0.
const zero = new Set([1]).union(new Set([-0]));
console.log([...zero].join(','));
console.log(Object.is([...zero][1], 0), Object.is([...zero][1], -0));

// The seven really are members of a Set and not of a Map.
console.log('union' in a, 'isDisjointFrom' in a);
console.log('union' in m, 'symmetricDifference' in m);
console.log(typeof a.union, typeof a.intersection, typeof a.isSubsetOf);
