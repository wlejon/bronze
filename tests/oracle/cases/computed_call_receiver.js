// `o[k](...)` — the receiver of a call through a COMPUTED member expression.
// ECMA-262 13.3.6.1 (EvaluateCall) evaluates the MemberExpression once and
// passes its base as the this value, and 13.3.3 makes no distinction between
// `o.m` and `o[k]` about that: both produce a Reference whose base is `o`.
//
// bronze passed no receiver at all through the second form, so `this` inside
// a method reached by `o[k]()` was undefined and every `this.x` in it read
// `undefined` — a silent wrong answer, and the one that shows up the moment a
// program takes an iterator by hand with `v[Symbol.iterator]()`.
//
// What is pinned:
//
// 1. `o['m']()`, `o[k]()` and `o[i]()` on an array all run with `o` as the
//    receiver, exactly as `o.m()` does.
// 2. The base is evaluated ONCE: a base with a side effect runs it a single
//    time, which is the difference between reading it for the callee and
//    reading it again for the receiver.
// 3. The KEY is evaluated after the base and before the arguments (13.3.3
//    evaluates the MemberExpression, then EvaluateCall evaluates Arguments).
// 4. `o?.[k]()` short-circuits on a nullish base without evaluating the key.

class Counter {
    constructor(start) {
        this.n = start;
    }

    bump(by) {
        this.n = this.n + by;
        return this.n;
    }
}

const c = new Counter(10);
console.log(c.bump(1));
console.log(c['bump'](2));
const key = 'bump';
console.log(c[key](3));
console.log(c.n);

// An array element that is a function, called through its index.
const ops = [
    function () {
        return this.length;
    }
];
console.log(ops[0]());

// 2 — the base runs once.
let baseReads = 0;
function base() {
    baseReads = baseReads + 1;
    return c;
}
console.log(base()[key](4), baseReads);

// 3 — base, then key, then arguments.
const order = [];
function noteBase() {
    order.push('base');
    return c;
}
function noteKey() {
    order.push('key');
    return 'bump';
}
function noteArg() {
    order.push('arg');
    return 5;
}
noteBase()[noteKey()](noteArg());
console.log(order);
console.log(c.n);

// 4 — a nullish base short-circuits before the key is evaluated.
const absent = null;
let keyReads = 0;
function countedKey() {
    keyReads = keyReads + 1;
    return 'bump';
}
console.log(absent?.[countedKey()](1), keyReads);
