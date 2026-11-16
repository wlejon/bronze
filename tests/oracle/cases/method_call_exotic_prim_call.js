// Method calls whose receiver is no plain object, answered by the method
// stub or the inline `f.call` arm, and the receivers each must hand to the
// generic path: string and number methods, array and typed-array methods
// (indexOf, splice, slice, subarray), RegExp methods, and `f.call` on plain
// functions, natives, class constructors, bound functions, proxies, functions
// with an own `call` and non-callables.
function strs(s, i) { return s.substring(i, i + 3) + '|' + s.charCodeAt(i) + '|' + s.split('a').length + '|' + s.trim().length; }
function nums(n) { return n.toString(16) + ':' + n.toFixed(2); }
const words = ['banana', '  alpha  ', 'cab', 'xyz'];
for (let r = 0; r < 3; r++) {
    for (let i = 0; i < words.length; i++) console.log(strs(words[i], 1));
    console.log(nums(255 + r), nums(3.14159 * r));
}

function arr(a, v) { return a.indexOf(v) + ',' + a.slice(1, 3).length + ',' + a.includes(v); }
const ta = new Float64Array([1, 2, 3, 4]);
const xs = [[1, 2, 3, 4], ta, [4, 3, 2, 1], ['a', 'b', 'c'], ta.subarray(1)];
for (let r = 0; r < 3; r++) for (const x of xs) console.log(arr(x, 3));

function spliceAll(a) {
    const out = [];
    while (a.length) out.push(a.splice(0, 2).join('+'));
    return out.join(' ');
}
console.log(spliceAll([1, 2, 3, 4, 5]), spliceAll(['x', 'y']));

// An array the method stub must not answer: own method, subclass, frozen.
const own = [1, 2, 3];
own.indexOf = () => 'own indexOf';
class MyArr extends Array { indexOf(v) { return 'my ' + super.indexOf(v); } }
const my = MyArr.from([5, 6, 7]);
for (const x of [[7, 8], own, my, [9, 7]]) console.log(x.indexOf(7));
const fr = Object.freeze([1, 2, 3]);
try { fr.splice(0, 1); } catch (e) { console.log('frozen splice', e instanceof TypeError); }

const re = /a(b)?/g;
for (let r = 0; r < 3; r++) { re.lastIndex = 0; console.log(re.test('xab'), re.exec('cab') && re.lastIndex); }

// f.call: compiled, native, class constructor, bound, proxy, own `call`.
function sloppyThis() { return this === undefined ? 'undef' : typeof this; }
function strictThis() { 'use strict'; return this === undefined ? 'undef' : typeof this; }
function add(a, b, c) { return (this && this.base || 0) + a + (b === undefined ? 100 : b) + (c === undefined ? 1000 : c); }
const hasOwn = Object.prototype.hasOwnProperty;
class K { constructor() { this.k = 1; } }
const bound = add.bind({ base: 5 }, 1);
const proxied = new Proxy(add, {});
const withOwn = function () { return 'body'; };
withOwn.call = function (t, x) { return 'own call ' + x; };
const callees = [add, hasOwn, bound, proxied, withOwn, sloppyThis, strictThis, K, 42];
function callIt(f, t, a, b) { return f.call(t, a, b); }
for (let r = 0; r < 2; r++) {
    for (const f of callees) {
        try { console.log(String(callIt(f, { base: 10, a: 1 }, 'a', 2))); } catch (e) { console.log('threw', e.constructor.name); }
    }
}
console.log(add.call({ base: 1 }, 2), add.call(null, 1, 2, 3), add.call(undefined), strictThis.call(undefined), strictThis.call(7));
console.log(Math.max.call(null, 3, 9, 4), [].concat.call([1], [2], 3).join());
