// BLOCKED: `Object.defineProperties` on a target that is not an ordinary
// object. bronze refuses a Proxy and an Array by name (`refuseObjectKind`,
// builtin_object.cpp) rather than answering.
//
// ECMA-262 20.1.2.3 step 1 is "If O is not an Object, throw a TypeError" — and
// a Proxy and an Array are both Objects, so the operation proceeds and reaches
// each target's own [[DefineOwnProperty]]:
//
//   A PROXY routes it through 10.5.6, which calls the handler's
//   `defineProperty` trap once per key, in the order 20.1.2.3.1 step 5 walks
//   them. Forwarding to the target behind the handler's back is the one thing
//   a proxy must never do, so bronze refuses instead of guessing — the trap is
//   the missing piece, not the member.
//
//   An ARRAY routes it through 10.4.2.1, where an index key that already exists
//   is an ordinary redefinition of that element: `{ value: 99 }` names only the
//   value, so 10.1.6.3 step 4 leaves the element's three attributes as the
//   array made them (writable, enumerable and configurable all true). A NAMED
//   key is an ordinary property, which on a new key completes to all-false.
//   bronze keeps an array's elements and `length` outside any shape, so there
//   is nothing a descriptor could be written to, which is what the refusal
//   says.
//
// The day either of those is built, this case starts passing and the promotion
// is forced. It is not about the descriptor-literal lowering: that lowering
// routes a non-ordinary target to the same runtime check the generic member
// makes, so both paths refuse here identically.

const calls = [];
const target = {};
const p = new Proxy(target, {
    defineProperty(t, k, d) {
        calls.push(k);
        return Reflect.defineProperty(t, k, d);
    },
});
Object.defineProperties(p, {
    a: { value: 1, enumerable: true },
    b: { value: 2 },
});
console.log('1', calls.join(','), target.a, target.b);

const arr = [10, 20, 30];
Object.defineProperties(arr, {
    1: { value: 99 },
    name: { value: 'n' },
});
console.log('2', arr.join(','), arr.length, arr.name);
console.log('3', JSON.stringify(Object.getOwnPropertyDescriptor(arr, '1')));
console.log('4', JSON.stringify(Object.getOwnPropertyDescriptor(arr, 'name')));
