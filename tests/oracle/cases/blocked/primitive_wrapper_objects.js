// BLOCKED: "`new String(...)` is unsupported: bronze has no primitive wrapper
// objects. Call String(x) for the conversion, which is exact." — and the same
// message for `new Boolean(...)`.
//
// docs/0030 decisions 4 and 6 landed `String` and `Boolean` as CONVERSION
// functions and refused their `new` forms by name. What is missing is the
// String and Boolean exotic OBJECT (ECMA-262 10.4.3 and 20.3): a heap object
// with a `[[BooleanData]]` or `[[StringData]]` internal slot, `typeof`
// "object", a prototype chain, and — for a String object — index properties
// and a non-writable `length` that 10.4.3.4 StringGetOwnProperty synthesises
// from the wrapped characters.
//
// Refused rather than approximated because the shape of `bronze_construct`
// makes the approximation SILENT: a native constructor returns a primitive,
// 13.3.5.1 discards any non-object return in favour of the plain instance, and
// the program receives `{}`. `new String("ab").length` read `undefined` before
// the refusal, which is a value lying about what it is.
//
// This belongs with the same value-model work as
// cases/blocked/object_intrinsic_prototypes: a wrapper is only useful once
// there is a real `String.prototype` for it to inherit from, and both need the
// property path to find members THROUGH a prototype instead of beside the
// value. `Number` needs the identical treatment and is not written here,
// because `Number` is a namespace object in bronze and not a constructor at
// all — that is its own gap, not this case's.
//
// What this case pins when it lands, from ECMA-262 22.1.1.1 (String as a
// constructor), 10.4.3 (String exotic objects), 20.3.1.1 (Boolean), 7.2.15
// (IsLooselyEqual) and 7.1.2 (ToBoolean):
//
// 1. A wrapper is an OBJECT: `typeof` is "object", not "string" or "boolean".
// 2. A String object carries the wrapped characters as index properties and a
//    `length` (10.4.3.4), so it indexes like the primitive it wraps.
// 3. `==` unwraps through ToPrimitive and `===` does not, so a wrapper is
//    loosely equal and strictly unequal to the primitive it wraps.
// 4. ToBoolean of an OBJECT is true without ever looking inside it. That is why
//    `new Boolean(false)` is truthy — the single most cited reason not to use
//    these constructors, and the reason a silent `{}` would be so damaging.
// 5. The conversion forms are unaffected and stay primitives, which is what
//    every real program calls.

const s = new String("ab");
console.log(typeof s, s.length, s[0], s[1]);
console.log(s == "ab", s === "ab");
console.log(s.valueOf(), String(s), s + "!");

const b = new Boolean(false);
console.log(typeof b, b.valueOf(), !!b);
console.log(b == false, b === false);

console.log(typeof "ab", typeof String("ab"), typeof Boolean(0));
