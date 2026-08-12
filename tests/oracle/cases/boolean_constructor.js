// The global `Boolean` as a CONVERSION function.
//
// From ECMA-262 20.3.1.1 (Boolean): the result is ToBoolean(value), and 7.1.2
// gives ToBoolean exactly six false results — undefined, null, +0, -0, NaN and
// the empty string, plus `false` itself. EVERYTHING else is true, and the
// interesting cases are the ones that look false and are not: the string "0",
// the string "false", an empty array and an empty object are all true, because
// ToBoolean of an object is true without ever consulting its contents.
//
// With no argument at all the parameter is `undefined`, so `Boolean()` is
// false — the same reading `String()` does NOT get (22.1.1.1 step 1 special-
// cases the empty argument list; 20.3.1.1 does not).
//
// 10.2.5 / 20.3.3.1: `Boolean.prototype.constructor` is `Boolean`, and 7.3.2
// GetV boxes a primitive receiver to find it — so `true.constructor` is the
// same object the bare name reads, exactly as `[].constructor` and
// `"".constructor` are. A name `Boolean.prototype` does NOT define is absent,
// and `undefined` is the language's own answer for that.

console.log(Boolean(true), Boolean(false));
console.log(Boolean(0), Boolean(-0), Boolean(NaN), Boolean(""));
console.log(Boolean(null), Boolean(undefined), Boolean());
console.log(Boolean(1), Boolean(-1), Boolean(Infinity), Boolean(0.5));
console.log(Boolean("0"), Boolean("false"), Boolean(" "));
console.log(Boolean([]), Boolean({}), Boolean(Boolean));
console.log(typeof Boolean, typeof Boolean(0));

console.log(true.constructor === Boolean, false.constructor === Boolean);
console.log(Boolean(1).constructor === Boolean, Boolean("").constructor === Boolean);
console.log(true.notAMember, false.alsoNotOne);
