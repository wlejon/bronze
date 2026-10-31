// new TypedArray(array) copies a leading run of plain numbers straight from
// the array's elements (fillFromNumberRun) and goes element by element from
// the first non-number on. Each case here is something that run must stop
// at, convert exactly as ToNumber + the kind's conversion would, or not take.
'use strict';

function show(label, ta) {
    const parts = [];
    for (let i = 0; i < ta.length; i++) {
        const v = ta[i];
        parts.push(typeof v === 'number' && v === 0 && 1 / v < 0 ? '-0' : String(v));
    }
    console.log(label, ta.constructor.name, ta.length, parts.join(','));
}

const kinds = [Float32Array, Float64Array, Uint8Array, Uint8ClampedArray,
               Int8Array, Int16Array, Uint16Array, Int32Array, Uint32Array];

const nums = [0, -0, 1.5, -1.5, 2.5, 255.5, 256, -1, 65535.75, 2147483648,
              -2147483649, 4294967296.5, NaN, Infinity, -Infinity, 1e40, 3.4028235677973366e38,
              1.0000001192092896, 5e-324, 0.1];
for (const K of kinds) show('nums', new K(nums));

// int32 values made by bitwise ops sit in the array as small ints.
const ints = [];
for (let i = 0; i < 12; i++) ints.push((i * 0x1234567) | 0);
for (const K of kinds) show('ints', new K(ints));

// A string mid-array ends the run; the rest converts by ToNumber.
for (const K of [Float32Array, Int16Array]) show('mixed', new K([1, 2, '3', 4.5, '0x10', null, true, undefined, 7]));

// valueOf that rewrites the source while the copy runs: elements after it
// are read after the write.
{
    const src = [1, 2, 0, 4, 5, 6];
    src[2] = { valueOf() { src[3] = 40; src[5] = 'x'; src.length = 5; return 3; } };
    show('mutate', new Float64Array(src));
    const src2 = [1, 2, 0, 4, 5, 6];
    src2[2] = { valueOf() { src2[4] = 50; return 3; } };
    show('mutate32', new Int32Array(src2));
}

// Holes read as undefined (NaN / 0); an array with its own iterator is iterated.
{
    const holey = [1, , 3, , 5];
    show('holes', new Float32Array(holey));
    show('holes8', new Uint8Array(holey));
    const custom = [1, { valueOf() { return 2; } }];
    custom[Symbol.iterator] = function* () { yield 7; yield 8.5; yield { valueOf() { return 9; } }; };
    show("ownIterator", new Float64Array(custom));
}


// A custom prototype and an Array subclass.
{
    const a = [1, 2, 3];
    Object.setPrototypeOf(a, { __proto__: Array.prototype, extra: 1 });
    show('customproto', new Uint16Array(a));
    class MyArr extends Array {}
    const m = MyArr.from([4.5, 5.5, -6.5]);
    show('subclass', new Int8Array(m));
}

// A BigInt array from numbers throws; from bigints it copies.
for (const K of [BigInt64Array, BigUint64Array]) {
    try { new K([1, 2]); console.log(K.name, 'no throw'); }
    catch (e) { console.log(K.name, e instanceof TypeError); }
    const b = new K([1n, -1n, 2n ** 64n + 3n]);
    console.log(K.name, Array.from(b, String).join(','));
}

// Long runs, so a copy loop of any width ends at the right place.
for (const n of [0, 1, 7, 8, 9, 63, 64, 65, 1000]) {
    const a = [];
    for (let i = 0; i < n; i++) a.push(i * 0.75 - 3);
    if (n > 3) a[n - 2] = 'y';
    const f = new Float32Array(a);
    const u = new Uint8ClampedArray(a);
    let s = 0, t = 0;
    for (let i = 0; i < f.length; i++) { s += Number.isNaN(f[i]) ? 1000 : f[i]; t += u[i]; }
    console.log('run', n, f.length, s, t);
}
