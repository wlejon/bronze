// The COMPUTED-read cache (runtime/elem_ic.h) and every way its answer can
// stop being the one it recorded.
//
// `o[k]` where k is a value now consults a (shape, key) keyed cache before the
// key is turned into a string. An entry is wrong the moment the receiver's
// shape changes, the prototype chain changes, or the SAME shape is asked about
// a DIFFERENT key — and the last of those is the one a property site never has
// to think about, because its key is a compile-time constant.
//
// Every scenario warms with a loop first, so a stale entry answers three
// hundred times rather than once.

// --- 1. a NUMBER key naming a string property ------------------------------
// The largest bucket in the three.js bill: a plain object whose own keys are
// integer-like strings, read with a number. 7.1.19 ToPropertyKey turns 32926
// into "32926", so this is a name lookup and not an element one.
const glMap = { 32926: 'src-alpha', 32823: 'dst-alpha', 2960: 'stencil' };
let a1 = '';
for (let i = 0; i < 300; i = i + 1) a1 = glMap[32926];
console.log(a1);
console.log(glMap[32823], glMap[2960]);
// The same object asked about a key it does not have: absent is not cached, so
// the add below must be seen immediately.
console.log(glMap[1] === undefined);
glMap[1] = 'one';
console.log(glMap[1]);

// --- 2. one shape, two keys, alternating -----------------------------------
// A single site rotating between two keys of one shape. Each read must answer
// about ITS key; an entry that checked only the shape would answer the other.
const pair = { alpha: 10, beta: 20 };
let sumAlt = 0;
for (let i = 0; i < 300; i = i + 1) {
    sumAlt = sumAlt + pair[i % 2 === 0 ? 'alpha' : 'beta'];
}
console.log(sumAlt);

// --- 3. one shape, MANY receivers ------------------------------------------
// The entry names a slot, not a value: two objects of one shape must read
// their own.
function mk(x, y) { return { px: x, py: y }; }
const objs = [mk(1, 2), mk(3, 4), mk(5, 6)];
let sumRecv = 0;
const kx = 'px';
for (let i = 0; i < 300; i = i + 1) sumRecv = sumRecv + objs[i % 3][kx];
console.log(sumRecv);

// --- 4. a freshly BUILT key string, equal but not the same object ----------
// The witness is a hash; the identity is the content. A string assembled at
// run time must hit the same entry as the literal that filled it.
const built = { position: 'P', normal: 'N' };
let a4 = '';
for (let i = 0; i < 300; i = i + 1) a4 = built['position'];
console.log(a4);
const madeKey = 'posi' + 'tion';
console.log(built[madeKey]);
console.log(built['norm' + 'al']);

// --- 5. the receiver's own shape changes -----------------------------------
const grow = { first: 1 };
let a5 = 0;
const kf = 'first';
for (let i = 0; i < 300; i = i + 1) a5 = grow[kf];
console.log(a5);
grow.second = 2;          // a transition: a new shape, so the entry cannot match
console.log(grow[kf], grow['second']);
grow.first = 99;          // same shape, new value: the slot must be re-read
console.log(grow[kf]);

// --- 6. the key appears and disappears on the RECEIVER ---------------------
const shadow = { base: 'own' };
let a6 = '';
for (let i = 0; i < 300; i = i + 1) a6 = shadow['base'];
console.log(a6);
delete shadow.base;
console.log(shadow['base'] === undefined);
shadow.base = 'again';
console.log(shadow['base']);

// --- 7. a PROTOTYPE hit, and the epoch that retires it ---------------------
class P7 {}
P7.prototype.tag = 'proto';
const r7 = new P7();
let a7 = '';
const k7 = 'tag';
for (let i = 0; i < 300; i = i + 1) a7 = r7[k7];
console.log(a7);
P7.prototype.tag = 'changed';        // same shape on the prototype, new value
console.log(r7[k7]);
r7.tag = 'own';                      // an own property SHADOWS the inherited one
console.log(r7[k7]);
delete r7.tag;
console.log(r7[k7]);

// --- 8. two links up, then a nearer link shadows ---------------------------
class A8 {}
A8.prototype.deep = 'A';
class B8 extends A8 {}
const r8 = new B8();
let a8 = '';
for (let i = 0; i < 300; i = i + 1) a8 = r8['deep'];
console.log(a8);
B8.prototype.deep = 'B';             // a NEARER holder: the cached depth is wrong
console.log(r8['deep']);

// --- 9. setPrototypeOf under a warm entry ----------------------------------
const base9a = { where: 'a' };
const base9b = { where: 'b' };
const r9 = Object.create(base9a);
let a9 = '';
for (let i = 0; i < 300; i = i + 1) a9 = r9['where'];
console.log(a9);
Object.setPrototypeOf(r9, base9b);
console.log(r9['where']);

// --- 10. an ACCESSOR reached by a computed read ----------------------------
// A getter is a call, and it must run on EVERY read: a cache that answered
// from a slot would call it once and repeat the first answer.
let calls10 = 0;
const acc = {
    get counted() { calls10 = calls10 + 1; return calls10; }
};
let last10 = 0;
const k10 = 'counted';
for (let i = 0; i < 300; i = i + 1) last10 = acc[k10];
console.log(calls10, last10);

// --- 11. a DICTIONARY receiver ---------------------------------------------
// Enough deletes to force the object out of its shape; its properties then
// live in a table and no slot number can name them.
const dict = { d0: 0, d1: 1, d2: 2, d3: 3, d4: 4, d5: 5 };
delete dict.d1;
delete dict.d3;
delete dict.d5;
let sumDict = 0;
for (let i = 0; i < 300; i = i + 1) sumDict = sumDict + dict['d4'];
console.log(sumDict);
dict.d4 = 40;
console.log(dict['d4']);

// --- 12. a PROXY receiver: every read is a trap ----------------------------
let traps12 = 0;
const px = new Proxy({ hidden: 'H' }, {
    get(t, k) { traps12 = traps12 + 1; return t[k]; }
});
let a12 = '';
for (let i = 0; i < 50; i = i + 1) a12 = px['hidden'];
console.log(traps12, a12);

// --- 13. a BOOLEAN key -----------------------------------------------------
// `false` is the property name "false", which three.js really does read.
const boolMap = { false: 'F', true: 'T' };
let a13 = '';
for (let i = 0; i < 300; i = i + 1) a13 = boolMap[false];
console.log(a13);
console.log(boolMap[true], boolMap[1 === 1]);

// --- 14. numeric key edges -------------------------------------------------
// -0 and 0 name ONE property ("0"); NaN, a fraction and a huge value name
// their own string forms. All of them are names on a plain object.
const edges = {};
edges[0] = 'zero';
edges[1.5] = 'frac';
edges[NaN] = 'nan';
edges[1e21] = 'huge';
let a14 = '';
for (let i = 0; i < 300; i = i + 1) a14 = edges[-0];
console.log(a14);
console.log(edges[0], edges[1.5], edges[NaN], edges[1e21]);
console.log(Object.keys(edges).join(','));

// --- 15. a STRING WRAPPER's synthesized properties -------------------------
// 10.4.3.5 gives a String object index properties and a `length` that live in
// no shape. A cache that recorded a slot for one would answer from the wrong
// place, so the walk must never offer one to record.
const sw = new String('ab');
let a15 = '';
for (let i = 0; i < 300; i = i + 1) a15 = sw[0];
console.log(a15, sw[1], sw['length'], sw[2] === undefined);

// --- 16. the value read is the one written, under a warm entry -------------
// The last check is the plainest: fill the entry, then write through the same
// key and read it back. A cache that returned a remembered VALUE rather than a
// remembered SLOT passes everything above and fails here.
const live = { v: 1 };
let a16 = 0;
for (let i = 0; i < 300; i = i + 1) a16 = live['v'];
console.log(a16);
for (let i = 0; i < 10; i = i + 1) {
    live.v = i * 2;
    a16 = live['v'];
}
console.log(a16);
