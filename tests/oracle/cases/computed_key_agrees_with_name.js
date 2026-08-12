// `o[k]` and `o.k` are ONE question, and every receiver kind must give it one
// answer. ECMA-262 evaluates both through the same 13.3.2.1 MemberExpression
// step — ToPropertyKey then [[Get]] — and knows nothing about whether the key
// was written down or computed; only an implementation can tell them apart,
// and only by having two code paths.
//
// bronze had two, and they had drifted: the computed path answered `undefined`
// for `arr[k]` naming "push", "length" or "constructor", and skipped the
// namespace check that makes an unbuilt `Math` member a named error. Each line
// below is written in the computed form precisely because the written form was
// never the one that was wrong.
//
// Derived from 23.1.3.20 (Array.prototype.push), 23.1.3.26 (slice), 10.4.2.4
// (Array length), 10.2.5 (the constructor back-pointer), 21.3.2.24 (Math.max),
// 21.3.1.6 (Math.PI), 7.1.19 (ToPropertyKey) and 23.2 (typed arrays).

const arr = [10, 20, 30];
const kPush = "push";
const kLength = "length";
const kCtor = "constructor";

console.log(typeof arr[kPush]);
console.log(arr[kLength]);
console.log(arr[kCtor] === Array);

// Called through the computed key, so the receiver has to survive the lookup.
arr[kPush](40);
console.log(arr.join(","));

// An index is still an index: ToPropertyKey turns 1 into "1", and 10.4.2 makes
// the canonical numeric string an element rather than a named property.
console.log(arr[1]);
console.log(arr["1"]);

const kSlice = "slice";
console.log(arr[kSlice](1, 3).join(","));

// A namespace object. `Math.fround` is a named hard error; before this case
// `Math[k]` for the same name was silently `undefined`, so the two spellings
// disagreed about whether an unbuilt member exists.
const kMax = "max";
console.log(Math[kMax](3, 9, 4));
console.log(Math["PI"] === Math.PI);

// A plain object, which is the receiver that always worked — pinned so a
// future rearrangement of the shared path cannot quietly cost it.
const o = { a: 1, b: 2 };
const kA = "a";
console.log(o[kA] + o["b"]);

// A typed array separates elements from members on the same receiver.
const ta = new Float32Array(3);
ta[0] = 1.5;
const kByteLength = "byteLength";
console.log(ta[kByteLength]);
console.log(ta[0]);
console.log(ta[kCtor] === Float32Array);
