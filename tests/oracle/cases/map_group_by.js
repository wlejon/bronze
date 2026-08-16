// 24.1.2.1 Map.groupBy, which is GroupBy (7.3.35) with keyCoercion
// "collection" — the SAME walk `Object.groupBy` takes (object_group_by.js pins
// that half), poured into a Map instead of into a null-prototype object.
//
// The coercion is the whole difference and it is what this file is for. A
// property key is a string or a symbol, so `Object.groupBy` reaches every key
// through ToString and an object key collapses to "[object Object]"; a
// collection key is compared by SameValueZero (7.2.10), so an OBJECT groups by
// identity, NaN is one key rather than none, and `-0` is stored as `+0` because
// 7.3.35 step 6.g runs CanonicalizeKeyedCollectionKey (24.5.1) over it. The
// zero lines pin that normalization from both sides: `has(-0)` and `has(0)` are
// both true because SameValueZero never separated them, and the key the map
// hands back is `+0`, which only the stored value can answer.
//
// Group order is first occurrence and element order is source order, exactly as
// in the object form: 7.3.35 appends a new group at the end of `groups`.
const m = Map.groupBy([1, 2, 3, 4, 5], (x) => (x % 2 === 0 ? "even" : "odd"));
console.log(m.size);
console.log([...m.keys()].join("|"));
console.log(m.get("odd").join(","));
console.log(m.get("even").join(","));

const key = {};
const byIdentity = Map.groupBy([1, 2, 3], (x) => (x % 2 ? key : "even"));
console.log(byIdentity.size, byIdentity.get(key).join(","), byIdentity.get("even").join(","));

const zeros = Map.groupBy([10, 20], (x) => (x === 10 ? -0 : 0));
console.log(zeros.size, zeros.has(0), zeros.has(-0));
for (const k of zeros.keys()) console.log(Object.is(k, 0), Object.is(k, -0));
console.log(zeros.get(0).join(","));

const nan = Map.groupBy([1, 2], () => NaN);
console.log(nan.size, nan.get(NaN).join(","));

// Any iterable, not only an array: step 4 is GetIterator.
const bySize = Map.groupBy(new Set(["a", "bb", "c"]), (w) => w.length);
console.log(bySize.size, bySize.get(1).join(","), bySize.get(2).join(","));

// The callback sees (value, index), and a throw from it leaves the exception
// for the caller rather than swallowing it (step 6.e closes the iterator and
// returns the abrupt completion).
const byIndex = Map.groupBy(["a", "b"], (x, i) => i);
console.log(byIndex.get(0).join(","), byIndex.get(1).join(","));
try {
  Map.groupBy([1], () => {
    throw new RangeError("stop");
  });
} catch (e) {
  console.log(e instanceof RangeError, e.message);
}
