// An array's ToPrimitive answer is `Array.prototype.join` (ECMA-262 20.1.3.7,
// 23.1.3.34, 23.1.3.15).
//
// Hint default asks `valueOf` first, 20.1.3.7 hands back the array, which is
// not a primitive, so the search carries on to `toString` — and 23.1.3.34
// makes that one `Array.prototype.join` with no separator whenever `join` is
// callable. That delegation is why `'' + [1, 2]` is "1,2" and not
// "[object Array]": 20.1.3.6's tag is what a program gets when NO nearer
// prototype defines `toString`, and `Array.prototype` defines one.
//
// A null and an undefined element are the empty string rather than their own
// text (23.1.3.15 step 4), and a nested array joins recursively — the two
// places a hand-rolled stand-in would have gone wrong, which is why both have
// a line here.
console.log('' + [1, 2]);
console.log(String([1, 2, 3]));
console.log(`${['a', 'b']}`);
console.log('' + []);
console.log('' + [null, undefined, 1]);
console.log('' + [1, [2, 3], 4]);
