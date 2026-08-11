// `delete o.k` — the operator, and the DICTIONARY MODE it forces underneath
// (docs/0019 decisions 1 and 2).
//
// An object's own properties are the nodes of a shape transition chain
// (docs/0009 decision 1), and a chain is exactly the wrong structure to
// remove from the middle of: every shape below the removed node would have
// the wrong `slot_index`, and shapes are immortal and shared between
// objects, so the repair could not be local to the one object being deleted
// from. So the first successful delete moves that object — and only that
// object — to a private table it can be removed from.
//
// What this pins, from ECMA-262 13.5.1 (the operator), 10.5.6 ([[Delete]]),
// 10.4.2.1 (an array's `length`) and 13.3.9 (delete over an optional chain):
//
// 1. `delete o.k` removes the property, so it differs observably from
//    `o.k = undefined`: `"k" in o` goes false and the key leaves
//    `Object.keys`.
// 2. It evaluates to a boolean, and to `true` even when the property was
//    never there — a missing property is already in the state delete wants,
//    and answering true without touching the object is also what keeps
//    `delete o.missing` from demoting a record to a dictionary.
// 3. Deleting does not disturb the order of the keys that remain, and a key
//    RE-ADDED after a delete is a new insertion: it goes to the end rather
//    than back to where it was. docs/0009's insertion order is by last
//    creation, and only a delete can make that distinction visible.
// 4. Deleting an own property can UNSHADOW an inherited one — the read then
//    reaches the prototype's. That is the sharpest reason delete cannot be a
//    write of undefined: writing undefined would shadow forever.
// 5. An array element deleted becomes a hole. `length` is a separate own
//    property and is not touched, so it stays 3 while index 1 reads
//    undefined.
// 6. `delete a?.b` is legal and short-circuits like any other chain, so a
//    nullish base makes it a no-op that still evaluates to true — 13.5.1.2
//    asks whether the operand produced a Reference, and a chain that stopped
//    early produced none. That is the one place a short-circuited chain is
//    NOT `undefined`.
const o = { a: 1, b: 2, c: 3 };
console.log(delete o.b);
console.log(o.b);
console.log("b" in o);
console.log(Object.keys(o).join(","));
o.b = 9;
console.log(Object.keys(o).join(","));
console.log(delete o.zzz);

function Base() {}
Base.prototype.shared = "proto";
const child = new Base();
child.shared = "own";
console.log(child.shared);
console.log(delete child.shared);
console.log(child.shared);

const arr = [1, 2, 3];
console.log(delete arr[1]);
console.log(arr.length);
console.log(arr[1]);

const nn = null;
console.log(delete nn?.gone);
console.log(delete o?.a);
console.log(Object.keys(o).join(","));
