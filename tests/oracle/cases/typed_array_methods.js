// `%TypedArray%.prototype` — the members ECMA-262 23.2.3 defines that bronze
// has built. A member it has not built is still a named hard error rather than
// `undefined`.
//
// The pair this case exists for is `subarray` (23.2.3.30) against `slice`
// (23.2.3.27): the first makes a view over the SAME buffer and the second
// copies into a new one, so a write through the result is visible in the
// original for one of them and not the other.
const v = new Float32Array([1, 2, 3, 4, 5, 6]);

const sub = v.subarray(1, 4);
console.log(sub, sub.byteOffset, sub.buffer === v.buffer);
sub[0] = 99;
console.log(v);

const sliced = v.slice(1, 4);
console.log(sliced, sliced.byteOffset, sliced.buffer === v.buffer);
sliced[0] = -1;
console.log(v[1], sliced[0]);

// 23.2.3.30 with a negative bound counts back from the end, as every relative
// index in the specification does.
console.log(v.subarray(-2));
console.log(v.slice(-2, -1));

// 23.2.3.9 fill returns the array itself, so it chains; the value is
// converted once, before the loop (step 3), by the element kind's rule.
console.log(v.fill(0, 4));
console.log(new Uint8Array(4).fill(-1));

// 23.2.3.6 copyWithin moves within one buffer, so overlapping ranges behave
// as though the source had been read first.
const w = new Int16Array([1, 2, 3, 4, 5]);
console.log(w.copyWithin(0, 2));
const overlap = new Int16Array([1, 2, 3, 4, 5]);
console.log(overlap.copyWithin(1, 0, 4));

// 23.2.3.26 set, from an array and from another typed array of a different
// element kind (which converts, element by element).
const target = new Int16Array(6);
target.set([10, 20, 30], 2);
console.log(target);
target.set(new Uint8Array([7, 8]));
console.log(target);
try {
  target.set([1, 2, 3], 5);
} catch (e) {
  console.log(e.name);
}

// 23.2.3.17 indexOf uses IsStrictlyEqual and 23.2.3.16 includes uses
// SameValueZero, so they differ at exactly one value: NaN.
const nums = new Float64Array([1, 0 / 0, 3]);
console.log(nums.indexOf(3), nums.indexOf(77), nums.indexOf(0 / 0));
console.log(nums.includes(3), nums.includes(77), nums.includes(0 / 0));

// Neither converts the NEEDLE: a needle of the wrong type answers by
// comparison — -1/false — never by conversion or a throw (a BigInt needle
// would be ToNumber's TypeError if one ran), and `includes(null)` on a
// zero-filled view is false, not the true a ToNumber(null) == 0 shortcut
// would answer. The fromIndex conversion still runs its side effects first:
// step 4 precedes the loop.
console.log(nums.indexOf("3"), nums.includes("3"), nums.lastIndexOf("3"));
console.log(nums.indexOf(3n), nums.includes(3n));
console.log(new Float64Array(1).includes(null), new Float64Array(1).includes(0));
let sawFromIndex = false;
console.log(nums.indexOf("3", { valueOf() { sawFromIndex = true; return 0; } }), sawFromIndex);

// 23.2.3.18 join. Every element is a number, so its text is ToString(Number) —
// which prints a stored -0 as "0", unlike console.log.
console.log(new Int8Array([1, -2, 3]).join("-"));
console.log(new Int8Array([1, -2, 3]).join());
console.log(new Float64Array(1).join(""));

// 23.2.3.21 map produces a view of the same element kind, so the callback's
// result is converted on the way in; 23.2.3.15 forEach produces nothing and
// sees (value, index, array).
console.log(new Int8Array([1, 2, 3]).map(function (x) { return x * 100; }));
let seen = "";
new Uint16Array([7, 8]).forEach(function (x, i, self) {
  seen = seen + "[" + i + ":" + x + "/" + self.length + "]";
});
console.log(seen);

// A method read off a typed array and called with no receiver is the
// TypeError of the ValidateTypedArray step, not a read of whatever `this`
// happened to be.
const detached = v.join;
try {
  detached();
} catch (e) {
  console.log(e.name);
}
