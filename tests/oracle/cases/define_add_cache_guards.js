// A `define.own.attr` site (a literal-descriptor `Object.defineProperty` or
// `defineProperties` key, a class field) repeats the add it last made for a
// receiver at the same shape, inline. Each section warms one site on one kind
// of receiver and then hands it something the inline add must not take, so the
// answer has to come from 10.1.6.3 (or 10.4.2.1, 10.5.6, 20.1.2.4) instead.

function attempt(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}

function desc(o, k) {
    const d = Object.getOwnPropertyDescriptor(o, k);
    if (d === undefined) return 'none';
    return (d.writable ? 'w' : '-') + (d.enumerable ? 'e' : '-') + (d.configurable ? 'c' : '-') + ':' + String(d.value);
}

// One site, `id`, defined the way three.js's Object3D defines it.
function stamp(o, v) {
    Object.defineProperty(o, 'id', { value: v });
    return o;
}

// 1. Warm on fresh literals, then the attributes and the value.
let sum = 0;
for (let i = 0; i < 300; i++) sum += stamp({}, i).id;
const one = stamp({}, 7);
console.log('1', sum, desc(one, 'id'), Object.keys(one).length, attempt(() => { 'use strict'; one.id = 1; }), one.id);

// 2. A receiver that already has the key: a redefinition of a configurable
// property changes its value and keeps the attributes the descriptor omits.
const has = { id: 1 };
stamp(has, 2);
console.log('2', desc(has, 'id'));

// 3. ... and of a non-configurable one with a different value: TypeError.
console.log('3', attempt(() => stamp(one, 8)), one.id, attempt(() => stamp(one, 7)));

// 4. Non-extensible, sealed and frozen receivers at the warm shape's parent
// shape: the add is refused.
const ne = Object.preventExtensions({});
const se = Object.seal({});
const fr = Object.freeze({});
console.log('4', attempt(() => stamp(ne, 1)), attempt(() => stamp(se, 1)), attempt(() => stamp(fr, 1)),
            desc(ne, 'id'), desc(fr, 'id'));

// 5. A receiver that is some object's prototype: the add is visible through
// the child, and a cache keyed on the child's chain sees it.
const proto = {};
const child = Object.create(proto);
function readId(o) { return o.id; }
for (let i = 0; i < 50; i++) readId(child);
stamp(proto, 42);
console.log('5', readId(child), desc(proto, 'id'), desc(child, 'id'));

// 6. Arrays, functions, typed arrays and proxies after the warm-up.
const arr = [1, 2];
stamp(arr, 3);
function fn() {}
stamp(fn, 4);
const ta = new Uint8Array(2);
stamp(ta, 5);
const log = [];
const px = new Proxy({}, {
    defineProperty(t, k, d) {
        log.push(k + ':' + d.value + ':' + d.writable + ':' + ('enumerable' in d));
        return Reflect.defineProperty(t, k, d);
    },
});
stamp(px, 6);
console.log('6', desc(arr, 'id'), arr.length, desc(fn, 'id'), desc(ta, 'id'), log.join(','), desc(px, 'id'));

// 7. Non-objects: the TypeError of 20.1.2.4 step 1, at the warm site.
console.log('7', attempt(() => stamp(1, 1)), attempt(() => stamp('s', 1)), attempt(() => stamp(undefined, 1)));

// 8. A descriptor field inherited from Object.prototype after the warm-up: the
// literal `{ value: v }` then has an inherited `writable`, and a later `get`
// makes every such literal a TypeError.
Object.prototype.writable = true;
const w = stamp({}, 9);
delete Object.prototype.writable;
Object.prototype.get = 5;
const g = {};
const gr = attempt(() => stamp(g, 10));
delete Object.prototype.get;
const after = stamp({}, 11);
console.log('8', desc(w, 'id'), gr, desc(g, 'id'), desc(after, 'id'));

// 9. Many keys from one call, past the inline slots, on receivers that start
// with and without an overflow block, with values the collector has to trace.
function many(o) {
    Object.defineProperties(o, {
        a: { value: { n: 1 }, enumerable: true },
        b: { value: [2], writable: true },
        c: { value: 'c', configurable: true },
        d: { value: 4, enumerable: true, writable: true, configurable: true },
        e: { value: null },
        f: { enumerable: true },
    });
    return o;
}
let shape = '';
for (let i = 0; i < 200; i++) {
    const o = i % 3 === 0 ? {} : i % 3 === 1 ? { p: 1, q: 2, r: 3, s: 4, t: 5 } : { p: 1 };
    many(o);
    if (i < 3) shape += Object.keys(o).join('') + '|';
    if (o.a.n + o.b[0] + o.d !== 7) shape += 'BAD';
}
const m = many({ p: 1, q: 2, r: 3, s: 4, t: 5 });
console.log('9', shape, desc(m, 'a').slice(0, 4), desc(m, 'b'), desc(m, 'c'), desc(m, 'd'), desc(m, 'e'), desc(m, 'f'));

// 10. Class fields are defines too (CreateDataPropertyOrThrow): a number field
// that later holds a string, and a base whose fields land on an object the
// base chose to return.
class F {
    x = 1;
    y = F.next++;
}
F.next = 0;
let fs = 0;
for (let i = 0; i < 100; i++) fs += new F().x + new F().y;
F.prototype.constructor = F;
class G {
    x = 'str';
    y = 2;
}
const fx = new F();
const gx = new G();
class R { constructor() { return R.target; } }
R.target = { pre: 1 };
class S extends R { z = 3; }
const s1 = new S();
const s2 = new S();
console.log('10', fs, fx.x, fx.y, gx.x, gx.y, s1 === s2, s1.z, Object.keys(s1).join(','),
            attempt(() => new S()), desc(R.target, 'z'));
Object.freeze(R.target);
console.log('11', attempt(() => new S()));
