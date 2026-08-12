// `Object.getPrototypeOf` of a PRIMITIVE, which ECMA-262 20.1.2.12 answers
// because its step 1 is ToObject and not a type test.
//
// Only `null` and `undefined` have no answer, and they are a TypeError for the
// reason 7.1.18 gives rather than for being "not an object" — every value below
// is not an object and every one of them has a prototype. bronze refused all of
// them alike, which was the wrong half of a right idea.
//
// It is the other half of `cases/object_intrinsic_prototypes`, which pins
// `Object.prototype` as a real object on the real chain: a string's members are
// now found by the ordinary prototype walk too, so the holder is a thing a
// program can name, compare and reach. An array and a function are still
// answered BESIDE the value, and `Object.getPrototypeOf` still says so by name
// for those — `blocked/get_prototype_of_number` is the same gap for a number.
//
// What this pins, from 20.1.2.12, 7.1.18 ToObject, 10.4.3 (String exotic
// objects) and 20.3 (Boolean):
//
// 1. A string primitive's prototype is `String.prototype` — the object, by
//    identity, not a copy — and every string in the program shares the one
//    intrinsic whatever it holds.
// 2. A boolean's is `Boolean.prototype`, on the same rule.
// 3. A String WRAPPER answers the same object as the primitive it wraps, which
//    is what makes `"x".toUpperCase` and `new String("x").toUpperCase` the same
//    function.
// 4. `String.prototype` is itself an ordinary object, so the chain continues to
//    `Object.prototype` and does not stop at an intrinsic.
// 5. `null` and `undefined` are the TypeError, because ToObject is where they
//    fail and it runs before any prototype is read.
console.log(Object.getPrototypeOf("x") === String.prototype);
console.log(Object.getPrototypeOf("") === Object.getPrototypeOf("abc"));
console.log(Object.getPrototypeOf(true) === Boolean.prototype);
console.log(Object.getPrototypeOf(new String("x")) === String.prototype);
console.log(Object.getPrototypeOf(String.prototype) === Object.prototype);

try {
  Object.getPrototypeOf(null);
  console.log("no throw");
} catch (e) {
  console.log("null:", e instanceof TypeError, e.name);
}
try {
  Object.getPrototypeOf(undefined);
  console.log("no throw");
} catch (e) {
  console.log("undefined:", e instanceof TypeError, e.name);
}
