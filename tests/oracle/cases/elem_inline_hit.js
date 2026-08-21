// The computed-read cache answered AT THE SITE (codegen-llvm/llvm_elem_cache.cpp).
//
// tests/oracle/cases/elem_ic_computed_read.js and elem_ic_computed_absent.js
// pin what the TABLE answers. This case pins what a second reader of that
// table must not get wrong — a reader that is generated code, cannot call
// anything, and re-derives every guard the probe makes:
//
//   * the two key kinds it speaks for (a number's bits, a boolean's 0/1) and
//     the ones it must hand back (a string, a symbol, an object);
//   * both invalidation words: the receiver's SHAPE, which every own add
//     transitions, and `bronze_proto_epoch`, which is the only thing that can
//     see a key appear on a prototype;
//   * the three entry states it must refuse — an accessor, a dictionary
//     receiver, a hit at depth greater than zero;
//   * the receiver kinds the table never speaks for, which the site's own
//     array and typed-array arms answer first and must keep answering.
//
// Every read goes through one function, so one SITE sees every shape and every
// key kind — a site that only ever saw one of each would pass this file
// without the cache being consulted at all.

function readN(o, k) {
    return o[k];
}

// --- number keys over a plain object ----------------------------------------
const tbl = {};
tbl[0] = 'a';
tbl[1] = 'b';
tbl[2] = 'c';
tbl[100] = 'h';
for (let i = 0; i < 300; i = i + 1) {
    readN(tbl, 0);
    readN(tbl, 1);
    readN(tbl, 100);
}
console.log(readN(tbl, 0), readN(tbl, 1), readN(tbl, 2), readN(tbl, 100));
console.log(readN(tbl, 3), readN(tbl, -1), readN(tbl, 0.5));

// -0 and +0 name ONE property. The witness is the double's BITS, so the two
// may land in two entries — two entries with one answer is a duplicate, and a
// duplicate is a miss at worst. Two ANSWERS would be the bug.
console.log(readN(tbl, -0), readN(tbl, 0), readN(tbl, 0) === readN(tbl, -0));

// NaN and the infinities are property NAMES, and NaN is the witness that is
// not equal to itself as a double while being perfectly equal as bits.
tbl[NaN] = 'nan';
tbl[Infinity] = 'inf';
tbl[-Infinity] = 'ninf';
for (let i = 0; i < 300; i = i + 1) {
    readN(tbl, NaN);
    readN(tbl, Infinity);
}
console.log(readN(tbl, NaN), readN(tbl, Infinity), readN(tbl, -Infinity));

// --- boolean keys ------------------------------------------------------------
const flags = {};
flags[true] = 'T';
flags[false] = 'F';
for (let i = 0; i < 300; i = i + 1) {
    readN(flags, true);
    readN(flags, false);
}
console.log(readN(flags, true), readN(flags, false), readN(flags, 1), readN(flags, 0));

// --- absence, and the two things that end it --------------------------------
const box = { a: 1 };
for (let i = 0; i < 300; i = i + 1) readN(box, 7);
console.log('absent', readN(box, 7));
box[7] = 'now';
console.log('present', readN(box, 7));

// A key arriving on the PROTOTYPE, which no receiver shape can see. Only the
// proto epoch can, which is why an absent entry carries one and an own-slot
// entry does not.
function Holder() {}
const inst = new Holder();
for (let i = 0; i < 300; i = i + 1) readN(inst, 42);
console.log('proto-absent', readN(inst, 42));
Holder.prototype[42] = 'fromProto';
console.log('proto-present', readN(inst, 42));

// setPrototypeOf: the unconditional bump, and it dictionaries the object too.
const p1 = { z: 'p1' };
const p2 = {};
p2[9] = 'p2nine';
const child = Object.create(p1);
for (let i = 0; i < 300; i = i + 1) readN(child, 9);
console.log('sp-before', readN(child, 9));
Object.setPrototypeOf(child, p2);
console.log('sp-after', readN(child, 9));

// --- an ACCESSOR is a call, and this path cannot make one -------------------
const acc = {};
let reads = 0;
Object.defineProperty(acc, '5', {
    get: function () {
        reads = reads + 1;
        return 'getter' + reads;
    }
});
for (let i = 0; i < 4; i = i + 1) readN(acc, 5);
console.log('accessor', readN(acc, 5), reads);

// --- a dictionary receiver has no shape-indexed slots -----------------------
const dict = { x: 1 };
dict[3] = 'three';
for (let i = 0; i < 300; i = i + 1) readN(dict, 3);
console.log('dict-before', readN(dict, 3));
delete dict.x;
console.log('dict-after', readN(dict, 3), dict.x);

// --- the receiver kinds this cache never speaks for -------------------------
const arr = [10, 20, 30];
const f32 = new Float32Array([1.5, 2.5]);
const fn2 = function () {};
fn2[1] = 'onfn';
const str = 'abc';
const m = new Map([[1, 'one']]);
for (let i = 0; i < 300; i = i + 1) {
    readN(arr, 1);
    readN(f32, 0);
    readN(fn2, 1);
    readN(str, 1);
}
console.log('kinds', readN(arr, 1), readN(arr, 9), readN(f32, 0), readN(f32, 9));
console.log('kinds2', readN(fn2, 1), readN(str, 1), readN(str, 9), readN(m, 1), m.get(1));
console.log('len', readN(arr, 'length'), readN(str, 'length'));

// --- a hit at depth 1, which the site hands back to the helper --------------
const base = {};
base[8] = 'basalt';
const derived = Object.create(base);
derived[4] = 'own4';
for (let i = 0; i < 300; i = i + 1) {
    readN(derived, 8);
    readN(derived, 4);
}
console.log('depth', readN(derived, 8), readN(derived, 4));
derived[8] = 'shadow';
console.log('shadow', readN(derived, 8), base[8]);

// --- one site, two shapes; one shape, two keys ------------------------------
const sa = { p: 1 };
sa[0] = 'S1';
const sb = { q: 1, r: 2 };
sb[0] = 'S2';
let out = '';
for (let i = 0; i < 300; i = i + 1) out = readN(sa, 0) + readN(sb, 0);
console.log('two-shapes', out);
let alt = '';
for (let i = 0; i < 300; i = i + 1) alt = readN(tbl, 0) + readN(tbl, 1) + readN(tbl, 2);
console.log('two-keys', alt);

// --- string keys, which stay with the helper --------------------------------
const named = { alpha: 1, beta: 2 };
const kA = 'al' + 'pha';
for (let i = 0; i < 300; i = i + 1) readN(named, kA);
console.log('string-key', readN(named, kA), readN(named, 'beta'), readN(named, 'gamma'));

// --- warmed, because a path taken once is not a path ------------------------
let n = 0;
for (let i = 0; i < 2000; i = i + 1) {
    if (readN(tbl, 0) === 'a') n = n + 1;
    if (readN(tbl, 55) === undefined) n = n + 10;
    if (readN(flags, false) === 'F') n = n + 100;
    if (readN(arr, 0) === 10) n = n + 1000;
}
console.log('warm', n);
