// `Object.defineProperties` on a Proxy. ECMA-262 20.1.2.3 step 1 is "If O is
// not an Object, throw a TypeError" — and a Proxy is an Object, so the
// operation proceeds and reaches the proxy's own [[DefineOwnProperty]],
// 10.5.6, which calls the handler's `defineProperty` trap once per key, in
// the order 20.1.2.3.1 step 5 walks them. Forwarding to the target behind
// the handler's back is the one thing a proxy must never do; the trap sees
// each key and the target is written only by the trap's own Reflect call.
//
// Promoted from cases/blocked/ when 10.5.6 landed (proxy_reflect.cpp); the
// array half of the original case is cases/array_reflect_members.js.

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
