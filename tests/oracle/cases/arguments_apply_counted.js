// `f.apply(t, arguments)` in a function whose arguments are only counted or
// forwarded: up to twelve arguments arrive as values and the forward makes no
// Array (il.h kForwardedArguments, `apply.args`); past that, the list. Every
// receiver the forward's fast arm refuses takes the method call.
const out = [];
const target = {
    tag: 't',
    take() {
        return this.tag + arguments.length + '[' + Array.prototype.map.call(arguments, (v) => String(v)).join(',') + ']';
    },
};
function fwd() { return target.take.apply(target, arguments); }
for (let n = 0; n <= 15; n++) {
    const a = [];
    for (let i = 0; i < n; i++) a.push(i % 3 === 0 ? 'x' + i : i % 3 === 1 ? i * 1.5 : null);
    out.push(fwd(...a));
}
console.log(out.join(' '));

// Source parameters beside the forward, reassigned: the list holds what the
// call passed (a strict function's arguments object is unmapped).
function withParams(a, b) {
    'use strict';
    a = 'A';
    return target.take.apply({ tag: 'p' }, arguments) + ':' + a + ':' + b + ':' + arguments.length;
}
console.log(withParams(), withParams(1), withParams(1, 2, 3), withParams(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13));

// `this` as a strict callee sees it: as passed.
function strictThis() { 'use strict'; return this === null ? 'null' : typeof this + ':' + String(this); }
function fwdThis(t) { return strictThis.apply(t, arguments); }
console.log(fwdThis(null), fwdThis(undefined), fwdThis(5), fwdThis('s'));

// Receivers the fast arm refuses: an own `apply`, a bound function, a native
// whose arity the count does not cover, a class, a non-callable, a proxy.
const own = function () { return 'own body'; };
own.apply = function (t, list) { return 'own apply:' + list.length + ':' + list[1]; };
function fwdOwn() { return own.apply(null, arguments); }
const bound = target.take.bind({ tag: 'b' }, 'pre');
function fwdBound() { return bound.apply(null, arguments); }
function fwdMax() { return Math.max.apply(Math, arguments); }
function fwdPush() { const arr = [9]; return Array.prototype.push.apply(arr, arguments) + ':' + arr.join(','); }
class Klass { constructor() { this.made = true; } }
function fwdClass() { return Klass.apply(null, arguments); }
function fwdAny(f) { return f.apply(null, arguments); }
const proxied = new Proxy(function () { return 'proxied' + arguments.length; }, {});
console.log(fwdOwn(1, 2), fwdBound(1, 2), fwdMax(), fwdMax(3), fwdMax(3, 8, 1), fwdPush(1, 2));
try { fwdClass(1); } catch (e) { console.log('class:', e instanceof TypeError); }
for (const bad of [undefined, null, 3, 'str', {}]) {
    try { fwdAny(bad); console.log('no throw'); } catch (e) { console.log(typeof bad, e instanceof TypeError); }
}
console.log(fwdAny(proxied, 1, 2), fwdAny(function () { return arguments.length; }, 1, 2, 3));

// A function that gains an own property changes its shape: the latched site
// must not take it for the bare one.
function later() { return 'later' + arguments.length; }
function fwdLater() { return later.apply(null, arguments); }
const before = [];
for (let i = 0; i < 300; i++) before.push(fwdLater(i, i));
later.apply = function () { return 'replaced'; };
console.log(before[0], before[299], fwdLater(1), fwdLater());

// Throws through the forward, inside and outside a handler; recursion.
function thrower() { throw new Error('boom' + arguments.length); }
function fwdThrow() { try { return thrower.apply(null, arguments); } catch (e) { return e.message; } }
function fwdThrowOut() { return thrower.apply(null, arguments); }
console.log(fwdThrow(1, 2), fwdThrow());
try { fwdThrowOut(1); } catch (e) { console.log('out', e.message); }
function depth(n) { return n === 0 ? arguments.length : depth.apply(null, [n - 1, n, n]) + depth2.apply(null, arguments); }
function depth2() { return arguments.length > 1 ? 1 : 0; }
console.log(depth(5), depth(3, 'x'));

// The receiver and `this` are evaluated once, in order.
const order = [];
const holder = { get fn() { order.push('fn'); return target.take; } };
function fwdOrder() { return holder.fn.apply((order.push('this'), { tag: 'o' }), arguments); }
console.log(fwdOrder(1), order.join(','));

// Hot loop over several shapes of callee, with the count and the list.
let acc = 0;
const callees = [function (a, b) { return a + b; }, Math.min, function () { return arguments.length; }];
function hot() { return callees[acc % 3].apply(null, arguments); }
for (let i = 0; i < 30000; i++) acc += hot(i, 1) | 0;
for (let i = 0; i < 300; i++) acc += hot(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14) | 0;
console.log(acc);
