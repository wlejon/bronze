// BLOCKED: an array's ToPrimitive answer is `Array.prototype.join`, and bronze
// has neither that nor the `Array.prototype.toString` that calls it.
//
// ToPrimitive itself is built (`cases/to_primitive`) and it reaches an array
// correctly: hint default asks `valueOf` first, 20.1.3.7 hands back the array,
// which is not a primitive, so the search carries on to `toString` — and
// 23.1.3.34 makes that one `Array.prototype.join` with no separator whenever
// `join` is callable, which is why `'' + [1, 2]` is "1,2" and not
// "[object Array]".
//
// Both of those are on rt_members.cpp's unimplemented list, so every line below
// is that prototype's named hard error today. That is the honest answer and the
// reason this case exists rather than an `[object Array]` that would look
// right: 20.1.3.6's tag is what a program gets when NO nearer prototype defines
// `toString`, and `Array.prototype` defines one.
//
// A null and an undefined element are the empty string rather than their own
// text (23.1.3.15 step 4), and a nested array joins recursively — the two
// places a hand-rolled stand-in would have gone wrong.
//
// Unblocking this means implementing `Array.prototype.join` and the
// `Array.prototype.toString` that delegates to it, and taking both off
// `kArrayMembers`.
console.log('' + [1, 2]);
console.log(String([1, 2, 3]));
console.log(`${['a', 'b']}`);
console.log('' + []);
console.log('' + [null, undefined, 1]);
console.log('' + [1, [2, 3], 4]);
