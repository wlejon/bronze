// `e.constructor` for the Error family — the 10.2.5 back-pointer that the
// provided classes never got.
//
// docs/0008 installs `prototype.constructor` on every function the program
// writes, which is what makes `new this.constructor()` work. The Error
// classes are built by the runtime rather than by lowering (docs/0020
// decision 7) and were built without it, so `e.constructor` was `undefined`
// on every error bronze raised and every error a program constructed. That
// is a silent wrong answer to the most idiomatic question there is about a
// caught value, and `e.constructor.name` threw on it.
//
// What this pins, from ECMA-262 10.2.5 (MakeConstructor step 6), 20.5.3.1
// (`Error.prototype.constructor`) and 20.5.6.3.1 (each NativeError's):
//
// 1. `e.constructor` is the class itself, for an error the runtime raised as
//    much as for one the program built — a `catch` cannot tell them apart
//    (docs/0020 decision 7), so this must not either.
// 2. Each class gets its OWN, so `new TypeError(…).constructor` is
//    `TypeError` and not the `Error` its prototype chains to. That is what
//    makes the property useful for discriminating a caught error.
// 3. It is a real constructor: `new (new Error("a").constructor)("b")` builds
//    an Error with the message it was given.
// 4. It is NOT enumerable — `Object.keys` of an instance and a `for-in` over
//    one must both stay empty, exactly as `name` and `message` on the
//    prototype already are.

try {
  null.x;
} catch (e) {
  console.log(e.constructor === TypeError, e.constructor === Error);
}

const e2 = new TypeError("x");
console.log(e2.constructor === TypeError, e2.constructor === Error);
console.log(new Error("y").constructor === Error);
console.log(
  new RangeError("r").constructor === RangeError,
  new SyntaxError("s").constructor === SyntaxError,
  new ReferenceError("q").constructor === ReferenceError
);

// The prototype carries it, and each class's prototype carries its own.
console.log(Error.prototype.constructor === Error, TypeError.prototype.constructor === TypeError);

// A real constructor, reached without naming the class.
const c = new Error("cl").constructor;
const made = new c("made");
console.log(made.message, made.name, made instanceof Error);

// Still an Error in every other way.
console.log(e2 instanceof TypeError, e2 instanceof Error, e2.name, e2.message);

// Non-enumerable, on the instance and on the prototype.
const seen = [];
for (const k in new TypeError("f")) {
  seen.push(k);
}
console.log(seen.length, Object.keys(new TypeError("k")).length, Object.keys(Error.prototype).length);
