// A double read once and used on a rarely taken path that needs it boxed
// (slerpFlat's `t === 0` stores into a generic receiver) and in the state of
// the exits after it: the box is made in the blocks that use it, not where the
// double is. The rare paths, the hot path and a leave after the reads all see
// the same values.
function lerp4(dst, src0, src1, t) {
    const x0 = src0[0], y0 = src0[1], z0 = src0[2], w0 = src0[3];
    const x1 = src1[0], y1 = src1[1], z1 = src1[2], w1 = src1[3];
    if (t === 0) {
        dst[0] = x0; dst[1] = y0; dst[2] = z0; dst[3] = w0;
        return 0;
    }
    if (t === 1) {
        dst[0] = x1; dst[1] = y1; dst[2] = z1; dst[3] = w1;
        return 1;
    }
    const s = 1 - t;
    dst[0] = x0 * s + x1 * t;
    dst[1] = y0 * s + y1 * t;
    dst[2] = z0 * s + z1 * t;
    dst[3] = w0 * s + w1 * t;
    return x0 + y1 + z0 + w1;
}
function main() {
    const a = new Float64Array([1.5, -2.25, 3, 0.125]);
    const b = new Float64Array([-4, 5.5, 6.75, 8]);
    const d = new Float64Array(4);
    let acc = 0;
    for (let i = 1; i < 200000; i++) {
        acc += lerp4(d, a, b, (i % 97) / 97 + 0.001);
        acc += d[0] + d[3];
    }
    console.log(acc.toFixed(6));
    // The rare paths, into another kind of receiver.
    const plain = [0, 0, 0, 0];
    console.log(lerp4(plain, a, b, 0), plain.join(','));
    console.log(lerp4(plain, a, b, 1), plain.join(','));
    // A source that is not a Float64Array: the reads leave with what they hold.
    console.log(lerp4(d, [1, 2, 3, 4], b, 0.5), Array.from(d).join(','));
    console.log(lerp4(d, a, [NaN, -0, Infinity, 1e300], 0.25), Array.from(d).join(','));
    console.log(lerp4(plain, a, ['x', 2, 3, 4], 0), plain.join(','));
}
main();
