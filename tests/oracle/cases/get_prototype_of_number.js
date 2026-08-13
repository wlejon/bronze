// `Object.getPrototypeOf` of a NUMBER, which is `Number.prototype` — a real
// object on the real chain, shared by every number in the program.
//
// Step 1 of ECMA-262 20.1.2.12 is ToObject, so a primitive HAS a prototype and
// only `null` and `undefined` fail (7.1.18). A string and a boolean answer with
// their intrinsic in `cases/get_prototype_of_primitive`; the two lines here
// that are not about a number's chain being real are about it being the SAME
// chain a wrapper and the language itself see.
//
// What this pins, from 20.1.2.12 and 21.1.3:
//
// 1. A number's prototype is `Number.prototype`, by identity, and every number
//    in the program shares the one intrinsic.
// 2. The chain continues past it to `Object.prototype`, so a number inherits
//    `hasOwnProperty` the same way every other value does.
// 3. `toFixed` read off the intrinsic is the same function object the number
//    answers with, which is what makes the prototype the real holder rather
//    than a second copy of the member table. A table would answer both reads
//    and could still fail this line; an object cannot.
// 4. A Number WRAPPER answers the same object as the primitive it wraps — the
//    fact `cases/get_prototype_of_primitive` pins for a String.
//
// `cases/number_prototype_chain` is where the members reached through that
// chain are pinned; this file is about the chain itself.

console.log(Object.getPrototypeOf(1) === Number.prototype);
console.log(Object.getPrototypeOf(0) === Object.getPrototypeOf(1.5));
console.log(Object.getPrototypeOf(Number.prototype) === Object.prototype);
console.log(Object.getPrototypeOf(1).toFixed === (1).toFixed);
console.log(Object.getPrototypeOf(new Number(1)) === Number.prototype);
