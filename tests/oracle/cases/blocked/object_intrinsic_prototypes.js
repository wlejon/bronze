// BLOCKED: `unsupported: Object.prototype is not implemented`, and behind it
// `unsupported: Object.getPrototypeOf of a plain object needs Object.prototype,
// which bronze does not provide`.
//
// docs/0022 landed most of the `Object` namespace and stopped at the members
// that need an INTRINSIC PROTOTYPE OBJECT to exist. bronze has none: a plain
// `{}` has no prototype at all, an array's methods are handed out by the
// property path rather than found on an `Array.prototype` a program can hold,
// and the same is true of strings, numbers and functions. That is a value-model
// decision (docs/0011 decision 2 made a builtin a bare function object, not a
// member of a prototype a program can reach), so `Object.getPrototypeOf({})` is
// a named error rather than the `null` that would be indistinguishable from
// `Object.create(null)`'s honest answer.
//
// Building it means: real `Object.prototype` / `Array.prototype` /
// `Function.prototype` / `String.prototype` / `Number.prototype` objects, every
// object's root shape pointing at the right one, the property path finding
// methods THROUGH them instead of beside them, and `Object.keys` and `for-in`
// staying unaffected because everything on them is non-enumerable. It is the
// chunk that makes monkey-patching work, and it is not a corner of a builtins
// chunk.
//
// `hasOwn`, `is` and `getOwnPropertyDescriptors` are here rather than built
// because they belong with that work: two of them are one-liners, and an
// unpinned builtin is how docs/0000's "plausible but wrong" bugs got in — they
// land with the case that can test them next to their neighbours.
//
// What this case pins when it lands, from ECMA-262 20.1.2.12
// (getPrototypeOf), 20.1.2.13 (hasOwn), 20.1.2.14 (is), 20.1.2.9
// (getOwnPropertyDescriptors) and 20.1.3 (Object.prototype):
//
// 1. A plain `{}` inherits from `Object.prototype`, and `Object.prototype`
//    itself inherits from nothing.
// 2. `Object.is` is SameValue, so it separates `0` from `-0` where `===` does
//    not, and joins NaN to itself where `===` does not.
// 3. `hasOwn` asks about OWN properties only, so an inherited one answers
//    false where `in` answers true.
// 4. A descriptor map reports every own property, with the three attributes a
//    plain assignment gives.
console.log(Object.getPrototypeOf({}) === Object.prototype);
console.log(Object.getPrototypeOf(Object.prototype));
console.log(Object.prototype.hasOwnProperty.call({ a: 1 }, "a"));

console.log(Object.is(NaN, NaN), NaN === NaN);
console.log(Object.is(0, -0), 0 === -0);
console.log(Object.is(1, 1), Object.is("a", "a"));

const child = Object.create({ inherited: 1 });
child.own = 2;
console.log(Object.hasOwn(child, "own"), Object.hasOwn(child, "inherited"));
console.log("inherited" in child);

const map = Object.getOwnPropertyDescriptors({ a: 1, b: 2 });
console.log(map.a.value, map.a.writable, map.a.enumerable, map.a.configurable);
console.log(Object.keys(map).join(","));
