// `Math.<fn>(...)` in a module whose taint scan proves `Math` pristine. The
// five bit-exact functions — sqrt, abs, floor, ceil, trunc — may compile to
// bare instructions, and the suite's dual-mode run is the proof obligation:
// every IEEE 754 edge (NaN, signed zero, infinities, negative operands) must
// come out byte-identical to the dynamic path's libm kernel. The other Math
// functions, the argument shapes the native form refuses (no args, extra
// args, a string, an optional link), and the constants are pinned alongside
// so the boundary of the fast path is itself a tested line.

const x = 2.25;
console.log(Math.sqrt(x * x), Math.sqrt(2), Math.sqrt(9));

console.log(Math.sqrt(-1), Math.sqrt(NaN), Math.sqrt(Infinity),
            Object.is(Math.sqrt(-0), -0), Math.sqrt(0));

console.log(Math.abs(-3.5), Math.abs(3.5), Object.is(Math.abs(-0), 0),
            Math.abs(-Infinity));

console.log(Math.floor(2.7), Math.floor(-2.1), Math.floor(-0.2),
            Object.is(Math.floor(-0), -0), Math.floor(7));

console.log(Math.ceil(2.1), Math.ceil(-2.7), Object.is(Math.ceil(-0.5), -0),
            Math.ceil(NaN));

console.log(Math.trunc(2.9), Math.trunc(-2.9), Object.is(Math.trunc(-0.9), -0),
            Math.trunc(Infinity));

// Arguments that are proven typed-element reads, in bounds and out: the
// out-of-bounds read is undefined, and the method's own ToNumber makes NaN.
const v = new Float64Array(2);
v[0] = 4; v[1] = -4;
console.log(Math.sqrt(v[0]), Math.abs(v[1]), Math.sqrt(v[5]),
            Math.floor(v[0] / 3));

// A const chain that is definitely numeric without being inference-proven:
// dx = 3.5, dsq = 12.25 + 3.75 = 16, every step exact in binary.
const dx = v[0] - 0.5;
const dsq = dx * dx + 3.75;
console.log(Math.sqrt(dsq));

// The shapes the native form refuses, agreeing through the dynamic path:
// no argument (ToNumber(undefined) = NaN), an ignored extra argument, a
// string argument, and both optional links.
console.log(Math.sqrt(), Math.sqrt(25, 999), Math.sqrt("49"),
            Math?.sqrt(64), Math.sqrt?.(81));

// Functions outside the bit-exact five keep the runtime kernel.
console.log(Math.round(2.5), Math.round(-2.5), Math.min(1, 2),
            Math.max(-0, 0));

// The constants the pristine proof types as numbers.
console.log(Math.PI, Math.SQRT2 === Math.sqrt(2));
