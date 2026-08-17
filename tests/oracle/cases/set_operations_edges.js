// The set operations' EDGES (ECMA-262 24.2.4 / 24.2.1.2 GetSetRecord): what
// makes an argument "set-like enough", in what order the checks fire, and what
// the seven do when the other side's `keys` or `has` misbehaves.
//
// Derived from ECMA-262:
//
// 1. 24.2.1.2 reads `size` FIRST and runs ToNumber on it. An absent `size` is
//    `undefined`, whose ToNumber is NaN, which step 4 makes a TypeError — so an
//    ARRAY is refused, because `length` is not `size`. A negative `size` is a
//    RangeError (step 6), which is a different error from the missing one.
// 2. The methods are read and checked AFTER the size, `has` before `keys`
//    (steps 7-10), so `{size: 1}` reports the missing `has` and
//    `{size: 1, has(){}}` the missing `keys`.
// 3. A non-object argument — including `null` and a string — is the TypeError of
//    step 1, before any read at all.
// 4. `this` must be a real Set (each member's step 2 is RequireInternalSlot for
//    [[SetData]]), so a Map is not a valid receiver even though it is a valid
//    ARGUMENT.
// 5. `size` is BELIEVED: it decides which side is walked and it answers the two
//    counting predicates outright. A set-like that lies about its size gives a
//    lying answer, and that is the language's behaviour rather than bronze's.
// 6. An error out of the other side's `has`, or out of its `keys` iterator,
//    propagates unchanged.
// 7. `isSupersetOf` and `isDisjointFrom` CLOSE the other side's iterator when
//    they short-circuit (24.2.4.12 step 6.e, 24.2.4.10 step 8.d.i).
// 8. ToNumber on `size` runs a `valueOf`, and truncation applies: a size of 2.9
//    is 2.
const a = new Set([1, 2, 3]);

function reason(fn) {
  try {
    fn();
    return 'no throw';
  } catch (e) {
    return e.name;
  }
}

// 1 & 2: the record's checks, in order.
console.log(reason(() => a.union([1, 2])));
console.log(reason(() => a.union({})));
console.log(reason(() => a.union({ size: -1, has() {}, keys() {} })));
console.log(reason(() => a.union({ size: 1 })));
console.log(reason(() => a.union({ size: 1, has() {} })));
console.log(reason(() => a.union({ size: 1, has: 5, keys() {} })));
console.log(reason(() => a.union({ size: 1, has() {}, keys: 5 })));

// 3: a non-object argument.
console.log(reason(() => a.union(null)));
console.log(reason(() => a.union(undefined)));
console.log(reason(() => a.union('abc')));
console.log(reason(() => a.union(3)));

// Every one of the seven refuses the same way.
const bad = [];
for (const name of ['union', 'intersection', 'difference', 'symmetricDifference',
  'isSubsetOf', 'isSupersetOf', 'isDisjointFrom']) {
  bad.push(reason(() => a[name]([])));
}
console.log(bad.join(','));

// 4: the receiver must be a Set. The method is taken off an INSTANCE and not
// off `Set.prototype`, which bronze has no object for (builtin_map.cpp) and
// refuses by name.
const detachedUnion = a.union;
const detachedSubset = a.isSubsetOf;
console.log(reason(() => detachedUnion.call(new Map(), new Set())));
console.log(reason(() => detachedSubset.call([1], new Set())));

// 5: a lying `size` changes which side is walked, and so what gets called.
const walked = [];
const liar = {
  size: 0,
  has(v) {
    walked.push(`has:${v}`);
    return v === 2;
  },
  keys() {
    walked.push('keys');
    return [2][Symbol.iterator]();
  },
};
// size 0 < this set's 3, so `keys` is walked rather than `has` being asked.
console.log([...a.intersection(liar)].join(','));
console.log(walked.join(' '));
// And the counting predicate takes it at its word.
console.log(a.isSupersetOf(liar), new Set().isSubsetOf(liar));

// 8: ToNumber and truncation on `size`.
const fractional = {
  size: { valueOf() { return 2.9; } },
  has(v) { return v === 1; },
  keys() { return [1][Symbol.iterator](); },
};
console.log([...a.intersection(fractional)].join(','));

// 6: errors from the other side.
const angryHas = { size: 9, has() { throw new RangeError('has'); }, keys() { return [][Symbol.iterator](); } };
console.log(reason(() => a.intersection(angryHas)));
console.log(reason(() => a.isSubsetOf(angryHas)));
function* angryKeys() {
  yield 1;
  throw new SyntaxError('keys');
}
const angry = { size: 0, has() { return false; }, keys: angryKeys };
console.log(reason(() => a.union(angry)));
console.log(reason(() => a.isSupersetOf(angry)));

// A `keys` that answers something that is not an object at all.
console.log(reason(() => a.union({ size: 0, has() {}, keys() { return 7; } })));
// ...or an object with no `next`.
console.log(reason(() => a.union({ size: 0, has() {}, keys() { return {}; } })));

// 7: the short-circuiting pair close the other side's iterator.
function* closing(tag) {
  try {
    yield 4;
    yield 5;
  } finally {
    console.log(`closed ${tag}`);
  }
}
console.log(a.isSupersetOf({ size: 0, has() { return false; }, keys() { return closing('super'); } }));
console.log(a.isDisjointFrom({ size: 0, has() { return false; }, keys() { return closing('disjoint'); } }));
// The generator here yields 4 and 5, neither in `a`, so `isDisjointFrom` runs
// to exhaustion and the `finally` runs for that reason instead.
