// `new String(x)` and `new Boolean(x)`: the String exotic object (10.4.3) and
// the Boolean exotic object (20.3), each a heap object with one internal slot —
// [[StringData]] or [[BooleanData]] — and a real intrinsic prototype on its
// chain.
//
// The prototypes are the load-bearing half, and they are why this case and
// `cases/string_index` are one piece of work. `String.prototype` and
// `Boolean.prototype` are now REAL objects, in the sense
// `cases/object_intrinsic_prototypes` made `Object.prototype` one: a program can
// hold them, and a primitive and a wrapper reach them by the same ordinary
// prototype walk. Before that a string's members were answered BESIDE the value
// from a table, which left a wrapper with nothing to inherit from — and left an
// index with nothing to fall through to.
//
// The `new` forms could not be approximated, which is why they were refused
// rather than half-built: a native constructor cannot see NewTarget through
// bronze's uniform calling convention, its body returns a primitive, and
// 13.3.5.1 discards a non-object return in favour of the plain instance. The
// program received `{}`, and `new String("ab").length` read `undefined`. So
// `bronze_construct` builds the wrapper INSTEAD of entering the body.
//
// `new Number(x)` is the third of these and lives in
// `cases/number_prototype_chain`, beside the intrinsic it needed: `Number` was
// a namespace object rather than a constructor when this case was written, so
// there was no `new` form to intercept.
//
// What this pins, from 22.1.1.1 (String as a constructor), 10.4.3 (String
// exotic objects), 20.3.1.1 (Boolean), 7.2.15 (IsLooselyEqual) and 7.1.2
// (ToBoolean):
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
