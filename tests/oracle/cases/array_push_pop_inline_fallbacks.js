// `a.push(v)` and `a.pop()` answered inline on an Array at the bare array
// shape, and every receiver that must take the builtin's full path instead:
// no room left, a hole at the end, an own `push`, a subclass, a frozen or
// sealed array, a non-array receiver with the method,
// and an index getter and setter on the chain.
function pushAll(a, n) {
    let r = 0;
    for (let i = 0; i < n; i++) r = a.push(i % 3 === 0 ? 'v' + i : i);
    return r;
}
function popAll(a) {
    const out = [];
    let v;
    while ((v = a.pop()) !== undefined) out.push(v);
    return out;
}

const a = [];
console.log(pushAll(a, 37), a.length, a[0], a[36], a.join(','));
console.log(popAll(a).join(','), a.length);
console.log([].pop(), [1].pop(), [1, 2].push(3));

// A hole at the end: pop reads it (undefined) and still shortens.
const h = [1, 2, , ];
h.length = 4;
console.log(h.pop(), h.length, h.pop(), h.length);

// An own push, a subclass with its own push, a plain object with one.
const own = [1, 2];
own.push = function (v) { return 'own:' + v; };
console.log(own.push(5), own.length);
class Stack extends Array {
    push(v) { return 'stack:' + super.push(v * 10); }
    pop() { return 'popped:' + super.pop(); }
}
const s = new Stack();
console.log(s.push(1), s.push(2), s.pop(), s.length);
const fake = { push(v) { return 'fake:' + v; }, pop() { return 'fakepop'; } };
console.log(fake.push(1), fake.pop());

// Frozen and sealed.
const fr = Object.freeze([1, 2]);
try { fr.push(3); } catch (e) { console.log('frozen push', e instanceof TypeError); }
try { fr.pop(); } catch (e) { console.log('frozen pop', e instanceof TypeError); }
const se = Object.seal([1, 2]);
try { se.push(3); } catch (e) { console.log('sealed push', e instanceof TypeError); }
try { se.pop(); } catch (e) { console.log('sealed pop', e instanceof TypeError, se.length); }

// The same call site sees an array, then a receiver it must not inline.
function callPush(x, v) { return x.push(v); }
const many = [[], [1], own, s, fake, [2, 3]];
for (const x of many) console.log(callPush(x, 7));

// The number-ness of the elements: a pushed string then arithmetic over them.
const nums = [1.5, 2.5];
nums.push(3);
let sum = 0;
for (const x of nums) sum += x;
nums.push('x');
console.log(sum, nums.join('|'));

// An index key on some prototype (the thread's index hazard): plain arrays
// still push and pop as before, and an array whose chain holds the index
// takes its setter and getter.
const P = Object.create(Array.prototype);
let seen = [];
Object.defineProperty(P, '2', {
    set(v) { seen.push('set ' + v); },
    get() { return 'got'; },
    configurable: true,
});
const c = [0, 1];
Object.setPrototypeOf(c, P);
console.log(c.push(9), c.length, seen.join(','), Object.keys(c).join(','));
const d = [0, 1, , ];
d.length = 3;
Object.setPrototypeOf(d, P);
console.log(d.pop(), d.length);
const e = [5, 6];
console.log(e.push(1), e.pop(), e.pop(), e.length, callPush(e, 4), e.join(','));
