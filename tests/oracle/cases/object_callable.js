// `Object` called WITHOUT `new` (20.1.1.1). Steps 2 and 3 make it two different
// operations under one name: given null or undefined — or nothing at all — it
// builds a new ordinary object, and given anything else it is ToObject, which
// returns an object argument UNCHANGED and wraps a primitive one.
//
// That identity rule is the reason the form is used: `Object(x) === x` is the
// cheapest "is this already an object?" in the language, and `Object(x)` is how
// pre-class code normalised an argument before reading properties off it.
//
// `Object` is a function object here, which is what 20.1.1 says it is: it has a
// [[Call]] whose body is 20.1.1.1, a [[Construct]] that reaches the same body
// through `bronze_construct`, and its statics are the own properties of a
// function rather than of a namespace object. So `typeof Object` is "function",
// `Object.name` is "Object" and `Object.length` is 1 — and the wrapper cases
// below are what tell the two halves of step 3 apart.

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
