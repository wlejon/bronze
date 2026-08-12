// BLOCKED: `unsupported: enumerating the own keys of a String object is not
// implemented` — and the same message with "testing" and "describing", from the
// four other operations that ask an object what its own keys are.
//
// `cases/primitive_wrapper_objects` built the String exotic object and
// `cases/string_index` pinned reading an index off one. Both go through the
// PROPERTY PATH, which consults 10.4.3.5 StringGetOwnProperty and answers a
// fresh one-code-unit string. What is missing is the other direction: 10.4.3.3
// OwnPropertyKeys, which reports those same index properties as own keys and
// puts them AHEAD of the ordinary ones.
//
// Refused rather than approximated because the approximation is silent and
// unanimous: a wrapper's shape carries no index properties, so `Object.keys`,
// `for-in`, `getOwnPropertyNames`, `hasOwnProperty`, `in` and
// `getOwnPropertyDescriptor` would every one of them report a String object as
// having no indices at all, which is a wrong answer rather than a missing one.
//
// What blocks it is one fact about `rtOwnKeysOrdered` (src/runtime/rt_object.cpp):
// it is handed a RAW ObjectHeader* and its whole contract is that it allocates
// nothing while walking, because shape keys are arena-interned and immortal. An
// index key for a wrapper is neither — interning one goes through a heap string,
// which allocates, which moves the header the walk is holding. Materialising
// these keys therefore means changing that function to take a rooted receiver,
// at every one of its call sites; that is the work this case is waiting for.
//
// What this pins when it lands, from 10.4.3.3 (OwnPropertyKeys), 10.4.3.4
// (StringCreate's `length`) and 10.4.3.5 (StringGetOwnProperty):
//
// 1. The own keys are the indices in ascending order and then `length`, which
//    is the order 10.4.3.3 states rather than creation order.
// 2. `Object.keys` sees the indices and not `length`, because 10.4.3.4 defines
//    `length` non-enumerable and 10.4.3.5 defines an index enumerable. That one
//    difference is why both lines below are here.
// 3. An index is a real own property, so `hasOwnProperty` and `in` answer true
//    for one inside the length and false for one past it.
// 4. Its descriptor is non-writable and non-configurable and enumerable — the
//    exact attribute set 10.4.3.5 names, and the reason no program can shadow
//    an index property or delete one.
// 5. `for-in` visits the indices and nothing else: everything on
//    `String.prototype` and `Object.prototype` above it is non-enumerable.

const s = new String("ab");

console.log(Object.keys(s).join(","));
console.log(Object.getOwnPropertyNames(s).join(","));

console.log(s.hasOwnProperty("0"), s.hasOwnProperty("length"), s.hasOwnProperty("2"));
console.log("0" in s, "length" in s, "2" in s);
console.log(s.propertyIsEnumerable("0"), s.propertyIsEnumerable("length"));

const d = Object.getOwnPropertyDescriptor(s, "0");
console.log(d.value, d.writable, d.enumerable, d.configurable);

const seen = [];
for (const k in s) seen.push(k);
console.log(seen.join(","));
