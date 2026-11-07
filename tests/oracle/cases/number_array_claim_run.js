// A kernel's constant-index reads behind `is.number_array` claims: when every
// receiver's claim holds, the read run's dense-array guard is not asked again.
// Each way a claim can fail must still read what the program reads: a hole, a
// non-Number element, an array too short, an array-like that is no Array, an
// element turned non-Number between calls, a length cut between calls, one
// array behind both receivers, and a collection between calls.

class Mat {
    constructor() {
        this.elements = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    }
    multiplyMatrices(a, b) {
        const ae = a.elements;
        const be = b.elements;
        const te = this.elements;
        const a11 = ae[0], a12 = ae[4], a13 = ae[8], a14 = ae[12];
        const a21 = ae[1], a22 = ae[5], a23 = ae[9], a24 = ae[13];
        const a31 = ae[2], a32 = ae[6], a33 = ae[10], a34 = ae[14];
        const a41 = ae[3], a42 = ae[7], a43 = ae[11], a44 = ae[15];
        const b11 = be[0], b12 = be[4], b13 = be[8], b14 = be[12];
        const b21 = be[1], b22 = be[5], b23 = be[9], b24 = be[13];
        const b31 = be[2], b32 = be[6], b33 = be[10], b34 = be[14];
        const b41 = be[3], b42 = be[7], b43 = be[11], b44 = be[15];
        te[0] = a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41;
        te[4] = a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42;
        te[8] = a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43;
        te[12] = a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44;
        te[1] = a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41;
        te[5] = a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42;
        te[9] = a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43;
        te[13] = a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44;
        te[2] = a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41;
        te[6] = a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42;
        te[10] = a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43;
        te[14] = a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44;
        te[3] = a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41;
        te[7] = a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42;
        te[11] = a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43;
        te[15] = a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44;
        return this;
    }
}

function fill(m, seed) {
    for (let i = 0; i < 16; i++) m.elements[i] = ((i * 7 + seed) % 11) - 5 + i / 4;
    return m;
}

function show(label, m) {
    console.log(label, m.elements.map((v) => (typeof v === "number" ? Math.round(v * 1000) / 1000 : String(v))).join(","));
}

const a = fill(new Mat(), 1);
const b = fill(new Mat(), 2);
const out = new Mat();
let acc = 0;
for (let i = 0; i < 3000; i++) {
    out.multiplyMatrices(a, b);
    acc += out.elements[i % 16];
}
console.log("warm", Math.round(acc * 1000) / 1000);
show("warm-out", out);

// A hole: the claim fails, the read of the hole is undefined (NaN after *).
{
    const h = fill(new Mat(), 3);
    delete h.elements[5];
    out.multiplyMatrices(h, b);
    show("hole", out);
    h.elements[5] = 2;  // filled again: the claim may hold once more
    out.multiplyMatrices(h, b);
    show("hole-filled", out);
}

// A non-Number element: a string multiplies by ToNumber, an object by valueOf.
{
    const s = fill(new Mat(), 4);
    s.elements[0] = "3";
    out.multiplyMatrices(s, b);
    show("string", out);
    let calls = 0;
    s.elements[0] = { valueOf() { calls++; return 2; } };
    out.multiplyMatrices(s, b);
    show("valueOf", out);
    console.log("valueOf-calls", calls);
}

// Too short: reads past the end are undefined.
{
    const short = new Mat();
    short.elements = [1, 2, 3, 4, 5, 6, 7, 8];
    out.multiplyMatrices(short, b);
    show("short", out);
}

// No Array at all: a typed array, a plain object with index keys, an
// array-like whose reads are getters.
{
    const f = new Mat();
    f.elements = new Float64Array(16).fill(0.5);
    out.multiplyMatrices(f, b);
    show("float64", out);
    const o = new Mat();
    o.elements = {};
    for (let i = 0; i < 16; i++) o.elements[i] = i;
    out.multiplyMatrices(o, b);
    show("object", out);
    let gets = 0;
    const g = new Mat();
    g.elements = {};
    for (let i = 0; i < 16; i++) Object.defineProperty(g.elements, i, { get() { gets++; return 1; } });
    out.multiplyMatrices(g, b);
    show("getters", out);
    console.log("getter-reads", gets);
}

// The same array turned non-Number, then back, between calls.
{
    const m = fill(new Mat(), 5);
    out.multiplyMatrices(m, b);
    show("before", out);
    m.elements[15] = null;
    out.multiplyMatrices(m, b);
    show("null", out);
    m.elements[15] = 1;
    out.multiplyMatrices(m, b);
    show("restored", out);
    m.elements.length = 10;
    out.multiplyMatrices(m, b);
    show("cut", out);
    m.elements.length = 16;
    out.multiplyMatrices(m, b);
    show("regrown", out);
}

// One array behind both receivers, and the output too.
{
    const m = fill(new Mat(), 6);
    out.multiplyMatrices(m, m);
    show("alias-in", out);
    m.multiplyMatrices(m, m);
    show("alias-out", m);
}

// A collection between calls: the arrays move, the claims' bits stay.
{
    const m = fill(new Mat(), 7);
    let keep = [];
    for (let r = 0; r < 5; r++) {
        for (let i = 0; i < 20000; i++) keep.push([i, { i }]);
        keep = [];
        out.multiplyMatrices(m, b);
    }
    show("gc", out);
}
