// Typed-array accesses at `base + k` and `base - k` off one shared base: the
// base's "is an integer index" answer is folded into the index each access
// range-checks, so one that is no index (a fraction, NaN, an infinity, a
// negative or a huge number) must still miss every access, a base near the
// view's end must hit only the offsets that are inside it, and -0 is index 0.

function pack(f32, u32, i16, f64, u8, index, v) {
    f32[index + 0] = v + 0.5;
    f32[index + 1] = v * 2;
    u32[index + 2] = v;
    u32[index + 3] = 4294967295 - v;
    i16[index + 4] = v * 3;
    f64[index + 5] = v / 3;
    u8[index + 6] = v + 250;
    f32[index - 1] = -v;
    u32[index - 2] = v + 7;
    f32[index + 7] = v * 1.5;
    return [f32[index + 0], f32[index + 1], u32[index + 2], u32[index + 3], i16[index + 4], f64[index + 5],
            u8[index + 6], f32[index - 1], u32[index - 2], f32[index + 7], f32[index + 2147483647],
            u32[index - 2147483648]].join(',');
}

function views() {
    const buf = new ArrayBuffer(96);
    return [new Float32Array(buf), new Uint32Array(buf), new Int16Array(buf), new Float64Array(buf),
            new Uint8Array(buf)];
}

const bases = [0, 1, 2, 3, 4, 5, 6, 10, 12, 16, 17, 22, 23, 24, 25, 40, 47, 48, 88, 95, 96, 97,
               -0, -1, -2, -7, -8, -2147483648, 2147483647, 2147483648, 4294967295, 4294967296, 4294967297,
               9007199254740992, -9007199254740992, 1e21, -1e21, 1e300, -1e300, 0.5, 1.5, -0.5, 2.9999999999,
               NaN, Infinity, -Infinity, 5e-324, 1.0000000000000002];

for (let round = 0; round < 3; round++) {
    for (const base of bases) {
        const vs = views();
        const out = pack(vs[0], vs[1], vs[2], vs[3], vs[4], base, 3 + round);
        console.log(Object.is(base, -0) ? '-0' : String(base), out, Array.from(vs[1]).join(','));
    }
}

// A loop over quads, as a batcher packs them, running off the end of the view.
function fill(f32, u32, n) {
    let index = 0;
    for (let q = 0; q < n; q++) {
        f32[index + 0] = q;
        f32[index + 1] = q + 0.25;
        u32[index + 2] = q * 65537;
        f32[index + 3] = f32[index + 0] * f32[index + 1];
        index += 4;
    }
    return index;
}
{
    const buf = new ArrayBuffer(4 * 18);
    const f32 = new Float32Array(buf), u32 = new Uint32Array(buf);
    console.log(fill(f32, u32, 7), Array.from(f32).join(','), Array.from(u32).join(','));
}

// A base that is a Number only some of the time.
function mixed(f32, base) {
    f32[base + 1] = 9;
    return f32[base + 1];
}
{
    const f32 = new Float32Array(4);
    console.log(mixed(f32, 1), mixed(f32, '1'), mixed(f32, 2.5), mixed(f32, undefined), mixed(f32, null),
                mixed(f32, { valueOf() { return 2; } }), Array.from(f32).join(','), f32['11'], f32['1.5']);
}
