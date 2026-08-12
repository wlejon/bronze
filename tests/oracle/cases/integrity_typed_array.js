// `Object.freeze` and `Object.seal` on a TYPED ARRAY, which is the one receiver
// in this family whose answer is a TypeError the language specifies rather than
// a limit bronze is conceding.
//
// The chain is short and worth following, because "freeze throws" reads like a
// bug until it is:
//
//  - 7.3.14 SetIntegrityLevel walks the receiver's own property keys and calls
//    DefinePropertyOrThrow on each — with `{ [[Configurable]]: false }` for
//    `seal`, and `{ [[Configurable]]: false, [[Writable]]: false }` for
//    `freeze`.
//  - A typed array is an integer-indexed exotic object, and its
//    [[DefineOwnProperty]] (10.4.5.3) refuses outright any descriptor for a
//    canonical numeric index that asks for `[[Configurable]]: false` or
//    `[[Writable]]: false`. Its elements are views onto a buffer; there is no
//    per-element attribute for the request to be recorded in.
//  - DefinePropertyOrThrow turns that `false` into a TypeError.
//
// So `Object.freeze` and `Object.seal` throw for a typed array with any
// elements at all, and the throw happens after [[PreventExtensions]] has
// already succeeded — 7.3.14 step 1 runs before the walk. `preventExtensions`
// itself has no such problem and is the one integrity operation a typed array
// accepts in the language.
//
// bronze cannot record [[Extensible]] for a typed array: a view keeps no
// property table, so `preventExtensions` on one is a hard error naming the kind
// rather than a bit written where nothing would read it back. That is why the
// predicates below are all safe to pin — the only route to a non-extensible
// typed array is refused, so `isExtensible` answering true is a fact and not a
// convention, and `isFrozen` / `isSealed` follow from 7.3.15 step 3 alone.
//
// The three lines after the throws are the point of the case: the view is
// unchanged and still writable. A freeze that had quietly done nothing would
// print exactly the same thing, which is why the throws above it are what make
// this pinning worth anything.

const view = new Uint8Array(3);
view[0] = 1;
view[1] = 2;
view[2] = 3;

console.log(Object.isExtensible(view), Object.isSealed(view), Object.isFrozen(view));

try {
  Object.freeze(view);
  console.log("freeze: no throw");
} catch (e) {
  console.log("freeze:", e instanceof TypeError, e.name);
}
try {
  Object.seal(view);
  console.log("seal: no throw");
} catch (e) {
  console.log("seal:", e instanceof TypeError, e.name);
}

console.log(Object.isExtensible(view), Object.isSealed(view), Object.isFrozen(view));
view[0] = 9;
console.log(view[0], view[1], view[2], view.length);

// The refusal is about the ELEMENTS, so it is the elements that decide: 7.3.14
// step 3 finds no keys on an empty view and never reaches DefinePropertyOrThrow.
// bronze stops earlier than that — it has nowhere to put the extensibility bit
// the successful path would set — so the empty view is a named hard error and
// is deliberately not exercised here.
const empty = new Uint8Array(0);
console.log(empty.length, Object.isExtensible(empty), Object.isFrozen(empty));
