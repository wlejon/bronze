// BLOCKED: `Uncaught TypeError: an object is not a function`.
//
// `Object` called WITHOUT `new` (20.1.1.1). Steps 2 and 3 make it two different
// operations under one name: given null or undefined — or nothing at all — it
// builds a new ordinary object, and given anything else it is ToObject, which
// returns an object argument UNCHANGED and wraps a primitive one.
//
// That identity rule is the reason the form is used: `Object(x) === x` is the
// cheapest "is this already an object?" in the language, and `Object(x)` is how
// pre-class code normalised an argument before reading properties off it.
//
// bronze builds the `Object` constructor object and answers its statics, but
// the value itself is not callable — the refusal above comes from the call
// path, which finds an object where it needs a function. Unblocking this means
// giving that object a [[Call]] whose body is 20.1.1.1, distinct from the
// [[Construct]] `new Object` already has.

console.log(Object(5).valueOf(), typeof Object(5), Object(5) instanceof Number);
console.log(Object("s").length, typeof Object("s"));
console.log(typeof Object(true), Object(true).valueOf());

const o = { a: 1 };
console.log(Object(o) === o, Object(o).a);

const arr = [1, 2];
console.log(Object(arr) === arr, Array.isArray(Object(arr)));

// null, undefined and no argument at all take step 2: a fresh ordinary object.
console.log(typeof Object(), Object.keys(Object()).length);
console.log(typeof Object(null), typeof Object(undefined));
console.log(Object.getPrototypeOf(Object(null)) === Object.prototype);
