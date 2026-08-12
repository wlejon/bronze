// BLOCKED: `unsupported: Object.getPrototypeOf of a number or a symbol needs
// Number.prototype / Symbol.prototype, which bronze does not provide`.
//
// This refusal is NEW, and it is the residue of a fix rather than a regression.
// `Object.getPrototypeOf` used to answer every primitive with one TypeError
// saying the argument was "not an object", which ECMA-262 20.1.2.12 contradicts
// twice over: step 1 is ToObject, so a primitive HAS a prototype, and only
// `null` and `undefined` fail (7.1.18). A string and a boolean now answer with
// the intrinsic (`cases/get_prototype_of_primitive`); a number and a symbol
// have no object to answer with, and this says so by name instead of returning
// a plausible one.
//
// The missing piece is `Number.prototype` as a real object on the real chain.
// `String.prototype` and `Boolean.prototype` are already that — a string
// reaches its members by the ordinary prototype walk — while a number's
// `toFixed`, `toString` and `toPrecision` are still handed out BESIDE the value
// by the property path (rt_prop_primitive.cpp), so there is no holder for a
// program to name. Returning `null` would deny a chain that demonstrably has
// methods on it, and returning `Object.prototype` would name the wrong holder;
// both are the silent wrong answer the house rules rank below a refusal.
//
// What this pins when Number.prototype lands, from 20.1.2.12 and 21.1.3:
//
// 1. A number's prototype is `Number.prototype`, by identity, and every number
//    in the program shares the one intrinsic.
// 2. The chain continues past it to `Object.prototype`, so a number inherits
//    `hasOwnProperty` the same way every other value does.
// 3. `toFixed` read off the intrinsic is the same function object the number
//    answers with, which is what makes the prototype the real holder rather
//    than a second copy of the member table.
// 4. A Number WRAPPER answers the same object as the primitive it wraps — the
//    fact `cases/get_prototype_of_primitive` pins for a String.
//
// When it passes, promote it and rewrite this header to say what it pins.

console.log(Object.getPrototypeOf(1) === Number.prototype);
console.log(Object.getPrototypeOf(0) === Object.getPrototypeOf(1.5));
console.log(Object.getPrototypeOf(Number.prototype) === Object.prototype);
console.log(Object.getPrototypeOf(1).toFixed === (1).toFixed);
console.log(Object.getPrototypeOf(new Number(1)) === Number.prototype);
