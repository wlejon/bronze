// BLOCKED: `Object.defineProperties` on a Proxy. bronze refuses it by name
// (`refuseObjectKind`, builtin_object.cpp) rather than answering.
//
// ECMA-262 20.1.2.3 step 1 is "If O is not an Object, throw a TypeError" — and
// a Proxy is an Object, so the operation proceeds and reaches the proxy's own
// [[DefineOwnProperty]], 10.5.6, which calls the handler's `defineProperty`
// trap once per key, in the order 20.1.2.3.1 step 5 walks them. Forwarding to
// the target behind the handler's back is the one thing a proxy must never
// do, so bronze refuses instead of guessing — the trap is the missing piece,
// not the member.
//
// The array half of this case promoted to cases/array_reflect_members.js when
// 10.4.2.1 landed. The day the trap is built, this one starts passing and the
// promotion is forced. It is not about the descriptor-literal lowering: that
// lowering routes a non-ordinary target to the same runtime check the generic
// member makes, so both paths refuse here identically.

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
