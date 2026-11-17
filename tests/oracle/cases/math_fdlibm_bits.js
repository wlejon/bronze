// Math.sin, cos, tan, exp, log, acos and atan2 to the last bit, over
// thousands of arguments of every magnitude: the engine computes them as V8
// does (fdlibm), where the platform's libm differs by one unit in the last
// place on a few percent of the arguments (tan, exp, log, acos). NaNs are
// compared as NaN, not by payload. Math.pow is fdlibm's too, but V8's is the
// platform's pow, which differs by platform in the last bit: its line is the
// results to 10 digits, and no argument is made with it (the wide magnitudes
// are a mantissa scaled by halving or doubling, which is exact).
const view = new DataView(new ArrayBuffer(8));
let seed = 7;
const next = () => {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed / 4294967296;
};
const args = [];
for (let i = 0; i < 3000; i++) args.push((next() - 0.5) * 20);
for (let i = 0; i < 1500; i++) args.push((next() - 0.5) * 2000);
for (let i = 0; i < 500; i++) args.push(next() * 1e-3);
for (let i = 0; i < 300; i++) args.push((next() - 0.5) * 1e9);
for (let i = 0; i < 500; i++) {
    let x = 1 + next();
    const e = Math.floor((next() - 0.5) * 60);
    for (let k = 0; k < e; k++) x *= 2;
    for (let k = 0; k > e; k--) x /= 2;
    args.push(x);
}
args.push(0, -0, 1, -1, Infinity, -Infinity, NaN, Math.PI, Math.PI / 2, 709.7, 709.8, -745.1, -746, 1e-310, 5e-324,
    0.5, -0.5, 0.6744, 1.0000001, -0.9999999);

const fns = {
    sin: Math.sin, cos: Math.cos, tan: Math.tan, exp: Math.exp, log: Math.log,
    acos: (x) => Math.acos(x * 0.09),
    atan2: (x) => Math.atan2(x, x * 0.7 - 1.3),
};
for (const name in fns) {
    const f = fns[name];
    let h = 0;
    let nan = 0;
    for (let i = 0; i < args.length; i++) {
        const r = f(args[i]);
        if (r !== r) { nan++; continue; }
        view.setFloat64(0, r);
        h = (Math.imul(h, 31) + view.getUint32(0)) | 0;
        h = (Math.imul(h, 31) + view.getUint32(4)) | 0;
    }
    console.log(name, args.length, nan, (h >>> 0).toString(16));
}
{
    let h = 0;
    let nan = 0;
    for (let i = 0; i < args.length; i++) {
        const r = Math.pow(Math.abs(args[i]) * 0.37, args[i] * 0.11);
        if (r !== r) { nan++; continue; }
        const t = r.toPrecision(10);
        for (let c = 0; c < t.length; c++) h = (Math.imul(h, 31) + t.charCodeAt(c)) | 0;
    }
    console.log('pow', args.length, nan, (h >>> 0).toString(16));
}
// The same kernels from a call the compiler sees with number arguments.
let s = 0;
for (let i = 0; i < args.length; i++) {
    if (!(Math.abs(args[i]) < 1e6)) continue;
    const x = args[i] * 1e-4;
    s += Math.exp(x) + Math.log(1 + Math.abs(x)) + Math.tan(x);
}
view.setFloat64(0, s);
console.log('loop', view.getUint32(0).toString(16), view.getUint32(4).toString(16));
