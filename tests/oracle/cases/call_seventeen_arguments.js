// A call with more than sixteen arguments. The fixed-operand call helpers
// stop at sixteen (`bronze_call_dynamic_16`, `bronze_construct_16`,
// `bronze_super_call_16` — abi/bronze_abi.h), and a longer operand list used
// to reach brass as one it had no helper for, which it answered with
// `undefined` and no call at all: `f(1, ..., 17)` was `undefined`, `new
// C(1, ..., 17)` was `undefined`, and a seventeen-argument `console.log`
// printed nothing. Such a call now travels the way a spread call does —
// its arguments as one array the runtime unpacks (`argsTakeArrayPath`,
// lower_pattern.cpp) — through every emitter that has a fixed form: the
// plain call, `new`, `super(...)`, a method call, a private method call, a
// tagged template with that many substitutions, and an inlined method.

function count() { return arguments.length; }
const sum = (...xs) => xs.reduce((a, b) => a + b, 0);

console.log(count(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16));
console.log(count(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17));
console.log(sum(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20));
console.log(Math.max(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17));
console.log(Math.min(17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0));

// Evaluation order and each argument's value both survive the array.
const seen = [];
function tap(v) { seen.push(v); return v; }
const dyn = count;
console.log(dyn(tap(1), tap(2), tap(3), tap(4), tap(5), tap(6), tap(7), tap(8), tap(9), tap(10),
                tap(11), tap(12), tap(13), tap(14), tap(15), tap(16), tap(17), tap(18)));
console.log(seen.join(','));

// A method call, with `this` intact.
const o = {
    n: 100,
    m() { return this.n + arguments.length; },
};
console.log(o.m(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17));
console.log([].concat(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18).length);
const arr = [0];
arr.push(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19);
console.log(arr.length, arr[19]);

// `new` and `super(...)`.
class Base {
    constructor() { this.argc = arguments.length; this.last = arguments[arguments.length - 1]; }
}
class Derived extends Base {
    constructor() {
        super(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21);
    }
}
const b = new Base(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17);
console.log(b.argc, b.last, b instanceof Base);
const d = new Derived();
console.log(d.argc, d.last, d instanceof Derived);

// A private method.
class P {
    #m() { return arguments.length; }
    run() { return this.#m(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17); }
}
console.log(new P().run());

// A tagged template with sixteen substitutions: the template object plus
// sixteen values is seventeen arguments.
function tag(strings, ...vals) { return strings.length + ':' + vals.length + ':' + vals[15]; }
const v = 'x';
console.log(tag`${v}${v}${v}${v}${v}${v}${v}${v}${v}${v}${v}${v}${v}${v}${v}${v}`);
console.log(tag`a${1}b${2}c${3}d${4}e${5}f${6}g${7}h${8}i${9}j${10}k${11}l${12}m${13}n${14}o${15}p${16}q${17}`);

// The argument past sixteen also reaches a callee that names its parameters.
function named(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q) { return q; }
console.log(named(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 'seventeenth'));
const viaVar = named;
console.log(viaVar(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 'again'));
