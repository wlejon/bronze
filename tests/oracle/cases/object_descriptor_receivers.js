// What the six `Object` members that need a property TABLE say about a receiver
// they will not take — and, more to the point, what they no longer say.
//
// `defineProperty`, `getOwnPropertyDescriptor`, `getOwnPropertyDescriptors`,
// `defineProperties`, `assign`, `hasOwn` and `getOwnPropertyNames` all gated on
// "is this a plain object", and answered a receiver that was not one with
// `TypeError: ... called on a value that is not an object`. For an array or a
// function that sentence is FALSE. An array is an object; what it is not is an
// object whose own keys live in a shape a descriptor could be written to. The
// message named the wrong fact, which is the same failure the integrity levels
// had — a predicate answering a different question than the step asks.
//
// The receivers split three ways now, and the split is the specification's:
//
//  - A plain object proceeds.
//  - A value that really is NOT an object gets the TypeError, and the message
//    is true of what it was given. Those are the lines below. Every one of them
//    is a TypeError ECMA-262 requires: 20.1.2.4 and 20.1.2.3 begin "If O is not
//    an Object, throw a TypeError", and the other five begin with ToObject,
//    whose only failures are `null` and `undefined` (7.1.18).
//  - An array, a function, a Map, a typed array — an object whose own storage
//    bronze cannot describe — is a hard error naming the KIND and the reason,
//    which cannot be pinned in a case that also prints, because it ends the
//    process. `tests/runtime/object_test.cpp` holds those.
//
// The primitives that ToObject SUCCEEDS on are not here, and the reason they
// are not is the opposite of what it once was: bronze answers them.
// `Object.hasOwn(1, "a")` is false and `Object.getOwnPropertyNames("ab")` is
// `["0","1","length"]`, which `cases/object_own_keys_primitive` pins. What is
// left in this file is the receiver ToObject itself rejects — the only one
// these five have a TypeError for at all.

function message(fn) {
  try {
    fn();
    return "no throw";
  } catch (e) {
    return e.name + ": " + e.message;
  }
}

// 20.1.2.4 / 20.1.2.3 step 1: any non-object at all.
console.log(message(() => Object.defineProperty(1, "a", { value: 1 })));
console.log(message(() => Object.defineProperty("s", "a", { value: 1 })));
console.log(message(() => Object.defineProperties(true, {})));

// The five that begin with ToObject: `null` and `undefined` are its only
// failures, so those are the only receivers whose TypeError is the language's.
console.log(message(() => Object.getOwnPropertyDescriptor(null, "a")));
console.log(message(() => Object.getOwnPropertyDescriptors(undefined)));
console.log(message(() => Object.getOwnPropertyNames(null)));
console.log(message(() => Object.hasOwn(undefined, "a")));
console.log(message(() => Object.assign(null, {})));

// The receiver that works, unchanged — because a diagnostic rewrite that also
// moved the accepting case would be the more dangerous edit of the two.
const o = {};
Object.defineProperty(o, "x", { value: 5, enumerable: true });
const d = Object.getOwnPropertyDescriptor(o, "x");
console.log(d.value, d.writable, d.enumerable, d.configurable);
console.log(Object.hasOwn(o, "x"), Object.hasOwn(o, "y"));
console.log(Object.getOwnPropertyNames(o).join(","));
console.log(JSON.stringify(Object.assign({ a: 1 }, { b: 2 })));
console.log(JSON.stringify(Object.getOwnPropertyDescriptors({ q: 1 })));
