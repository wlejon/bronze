// Math.asin, Math.atan and Math.atan2 to the last bit: the engine computes
// them as V8 does (fdlibm), where the platform's libm differs by one unit in
// the last place on about one argument in eight.
const view = new DataView(new ArrayBuffer(8));
const hex = (d) => {
    view.setFloat64(0, d);
    return view.getUint32(0).toString(16).padStart(8, '0') + view.getUint32(4).toString(16).padStart(8, '0');
};

// A fixed stream of doubles from a 32-bit LCG.
let seed = 12345;
const next = () => {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed / 4294967296;
};

let h = 0;
const mix = (d) => {
    view.setFloat64(0, d);
    h = (Math.imul(h, 31) + view.getUint32(0)) | 0;
    h = (Math.imul(h, 31) + view.getUint32(4)) | 0;
};
for (let i = 0; i < 20000; i++) {
    const y = next() * 2 - 1, x = next() * 2 - 1;
    mix(Math.atan2(y, x));
    mix(Math.atan2(y * 1000, x));
    mix(Math.atan(x * 4));
    mix(Math.atan(y / (x * x)));
    mix(Math.asin(y));
}
console.log(h);

// One value from each of the kernels' ranges.
const row = (f, xs) => console.log(xs.map((x) => hex(f(x))).join(' '));
row(Math.atan, [0.1, 0.4375, 0.5, 0.6875, 1, 1.1875, 1.5, 2.4375, 3, -0.25, -0.9, -5, 1e-10, 1e20, -1e30]);
row(Math.asin, [0.1, 0.3, 0.4999, 0.5, 0.6, 0.8, 0.975, 0.98, 0.999999, 1, -1, -0.25, -0.7, -0.99, 1e-9]);
const pairs = [[0.3, 0.7], [-0.3, 0.7], [0.3, -0.7], [-0.3, -0.7], [1, 1], [2.5, 1], [1e-10, 3], [5, 1e-12],
    [0.6, 0.9], [1.3, 0.7], [7, 2], [1e80, 1e-80], [1e-80, -1e80]];
console.log(pairs.map(([y, x]) => hex(Math.atan2(y, x))).join(' '));

// Zeros, infinities and NaN.
const special = [0, -0, 1, -1, Infinity, -Infinity, NaN];
for (const y of special) console.log(special.map((x) => Math.atan2(y, x)).join(' '));
console.log(special.map((x) => Math.atan(x)).join(' '));
console.log(special.map((x) => Math.asin(x)).join(' '), Math.asin(1.5), Object.is(Math.asin(-0), -0));
console.log(Object.is(Math.atan2(-0, 3), -0), Object.is(Math.atan2(0, 3), 0), Object.is(Math.atan(-0), -0));

// The rotation-matrix-to-Euler step three.js runs for every animated bone.
const e = [0.36, 0.48, -0.8, -0.8, 0.6, 0, 0.48, 0.64, 0.6];
console.log(hex(Math.asin(Math.max(-1, Math.min(1, e[6])))), hex(Math.atan2(-e[7], e[8])), hex(Math.atan2(-e[3], e[0])));
