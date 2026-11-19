function testInstanceofHardened() {
    class A {
        constructor(v) { this.v = v; }
    }
    class B extends A {
        constructor(v, w) { super(v); this.w = w; }
    }
    Object.freeze(A);
    Object.freeze(B);

    const probes = [
        new Float32Array(2),
        new Uint8Array(2),
        new Int32Array(2),
        new Uint8ClampedArray(2),
        [1, 2],
        new Map(),
        new Set(),
        /abc/,
        new Error("err"),
        new Number(5),
        new B(1, 2),
        new A(3),
        { x: 1 },
        null,
        undefined,
        42,
        "str",
    ];

    function checkAll(x) {
        return (
            (x instanceof Float32Array ? 1 : 0) +
            (x instanceof Uint8Array ? 2 : 0) +
            (x instanceof Int32Array ? 4 : 0) +
            (x instanceof Uint8ClampedArray ? 8 : 0) +
            (x instanceof Array ? 16 : 0) +
            (x instanceof Map ? 32 : 0) +
            (x instanceof Set ? 64 : 0) +
            (x instanceof RegExp ? 128 : 0) +
            (x instanceof Error ? 256 : 0) +
            (x instanceof Number ? 512 : 0) +
            (x instanceof A ? 1024 : 0) +
            (x instanceof B ? 2048 : 0)
        );
    }

    let sum = 0;
    for (let i = 0; i < 200; i++) {
        for (let j = 0; j < probes.length; j++) {
            sum += checkAll(probes[j]);
        }
    }
    const masks = probes.map(checkAll).join(",");

    class Base {}
    class Sub extends Base {}
    const sub = new Sub();
    const isBase = (o) => o instanceof Base;
    let beforeOverride = 0;
    for (let i = 0; i < 200; i++) {
        if (isBase(sub)) beforeOverride++;
    }
    Object.defineProperty(Base, Symbol.hasInstance, {
        value() { return false; },
        configurable: true,
    });
    const afterOverride = isBase(sub);

    return sum + "|" + masks + "|" + beforeOverride + "|" + afterOverride;
}

function setLenStrict(target, val) {
    "use strict";
    target.length = val;
}

function testTruncate() {
    let ok = 0;
    for (let i = 0; i < 150; i++) {
        const a = [1, 2, 3, 4, 5];
        setLenStrict(a, 0);
        if (a.length === 0) ok++;

        const b = [10, 20, 30, 40];
        setLenStrict(b, -0);
        if (b.length === 0) ok++;

        const c = [1.5, 2.5, 3.5, 4.5, 5.5];
        setLenStrict(c, 2);
        if (c.length === 2 && c[0] === 1.5 && c[1] === 2.5) ok++;

        setLenStrict(c, c.length);
        if (c.length === 2 && c[1] === 2.5) ok++;

        // Regrow after truncation: newly exposed slots must be holes, not stale numbers.
        setLenStrict(c, 6);
        if (c.length === 6 && !(2 in c) && c[2] === undefined && !(5 in c) && c[1] === 2.5) ok++;

        // Sparse write after truncation.
        const d = [1, 2, 3, 4, 5];
        setLenStrict(d, 2);
        d[8] = 9;
        if (d.length === 9 && !(3 in d) && d[3] === undefined && d[8] === 9) ok++;

        // push / unshift / splice after truncation.
        const e = [1, 2, 3, 4];
        setLenStrict(e, 0);
        e.push(10, 20);
        e.unshift(5);
        e.splice(1, 0, 7);
        if (e.length === 4 && e[0] === 5 && e[1] === 7 && e[2] === 10 && e[3] === 20) ok++;

        // Demote to object elements after NumberBlock truncation + allocate churn for GC.
        const f = [1, 2, 3, 4];
        setLenStrict(f, 1);
        f.push({ tag: "obj", n: i });
        let churn = null;
        for (let g = 0; g < 8; g++) churn = { g, arr: [g, g + 1] };
        if (f[0] === 1 && f[1].tag === "obj" && f[1].n === i && f.length === 2 && churn.g === 7) ok++;

        // Plain object `.length = n` in same polymorphic site.
        const o = { length: 10, x: i };
        setLenStrict(o, 3);
        if (o.length === 3 && o.x === i) ok++;
    }

    // Invalid lengths must throw RangeError and leave length unchanged.
    const badLengths = [-1, NaN, 1.5, Infinity, -Infinity];
    let rangeErrors = 0;
    for (let i = 0; i < 40; i++) {
        for (let j = 0; j < badLengths.length; j++) {
            const arr = [1, 2, 3, 4];
            try {
                setLenStrict(arr, badLengths[j]);
            } catch (e) {
                if (e instanceof RangeError && arr.length === 4) rangeErrors++;
            }
        }
    }

    // Frozen and sealed arrays must reject .length = 0 in strict mode.
    const frozen = Object.freeze([1, 2, 3]);
    const sealed = Object.seal([1, 2, 3]);
    let frozenRejected = 0;
    let sealedRejected = 0;
    for (let i = 0; i < 20; i++) {
        try {
            setLenStrict(frozen, 0);
        } catch (e) {
            if (e instanceof TypeError && frozen.length === 3) frozenRejected++;
        }
        try {
            setLenStrict(sealed, 0);
        } catch (e) {
            if (e instanceof TypeError && sealed.length === 3) sealedRejected++;
        }
    }

    // ValueBlock truncation and regrowth.
    let valOk = 0;
    for (let i = 0; i < 40; i++) {
        const v = [{ a: 1 }, { a: 2 }, { a: 3 }];
        setLenStrict(v, 1);
        setLenStrict(v, 3);
        if (v.length === 3 && v[0].a === 1 && !(1 in v) && v[1] === undefined && !(2 in v)) valOk++;
    }

    return ok + "|" + rangeErrors + "|" + frozenRejected + "|" + sealedRejected + "|" + valOk;
}

function testLicmDenseGrowth() {
    const src = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    let copyPushSum = 0;
    let copyIdxSum = 0;
    for (let r = 0; r < 100; r++) {
        const dst = [];
        for (let i = 0; i < src.length; i++) dst.push(src[i] * 2);
        const dst2 = [];
        for (let i = 0; i < src.length; i++) dst2[i] = src[i] + 1;
        if (r === 99) {
            for (let i = 0; i < dst.length; i++) copyPushSum += dst[i];
            for (let i = 0; i < dst2.length; i++) copyIdxSum += dst2[i];
        }
    }

    // Self-aliasing push: reads and pushes to the same array inside the loop.
    let selfStr = "";
    for (let r = 0; r < 50; r++) {
        const a = [1, 2, 3];
        for (let i = 0; i < 10; i++) a.push(a[i] + 1);
        if (r === 49) selfStr = a.length + ":" + a.join(",");
    }

    // Empty-start push-then-read: b starts empty, pushed before b[i] read in same iter.
    let pushReadSum = 0;
    let pushReadLen = 0;
    for (let r = 0; r < 50; r++) {
        const b = [];
        let s = 0;
        for (let i = 0; i < 20; i++) {
            b.push(i * 3);
            s += b[i];
        }
        pushReadSum = s;
        pushReadLen = b.length;
    }

    return copyPushSum + "|" + copyIdxSum + "|" + selfStr + "|" + pushReadSum + ":" + pushReadLen;
}

function testSpreadAndRest() {
    // 1. Inline double slots, object slots, boxed slots, mixed shapes, empty rest.
    let total = 0;
    let emptyKeys = 0;
    for (let i = 0; i < 150; i++) {
        const src = { a: i * 1.5, b: { nested: i }, c: i % 2 === 0 ? i : "s" + i, d: 4.25 };
        const copy = { ...src };
        const { a, b, ...rest } = src;
        total += copy.a + b.nested + rest.d + (typeof rest.c === "number" ? rest.c : rest.c.length);

        const { a: ea, b: eb, ...empty } = { a: 1, b: 2 };
        emptyKeys += Object.keys(empty).length + ea + eb;
    }

    // 2. Dynamically computed (non-immortal) excluded keys.
    let compSum = 0;
    for (let i = 0; i < 100; i++) {
        const obj = { k0: 1, k1: 2, k2: 3, keep: i };
        const dyn = "k" + (i % 3);
        const { [dyn]: dropped, ...rem } = obj;
        compSum += dropped + rem.keep + Object.keys(rem).length;
    }

    // 3. Old-generation target overwrite via Object.assign under allocation churn.
    const oldTarget = { a: 0, b: { k: -1 }, c: 0.5 };
    for (let g = 0; g < 6000; g++) {
        const tmp = [g, g + 1, g + 2, g + 3];
        if (tmp[0] === -1) return "unreachable";
    }
    let oldOk = 0;
    for (let i = 0; i < 100; i++) {
        const patch = { a: { nested: i }, b: { m: i * 2 }, c: i * 1.5 };
        Object.assign(oldTarget, patch);
        // More young allocations to push GC checks after young->old reference writes.
        const junk = { p: [i, i + 1], q: { r: i } };
        if (oldTarget.a.nested === i && oldTarget.b.m === i * 2 && oldTarget.c === i * 1.5 && junk.q.r === i) {
            oldOk++;
        }
    }

    // 4. Slot representation generalization: double slot generalized to string.
    let genOk = 0;
    for (let i = 0; i < 80; i++) {
        const t = {};
        Object.assign(t, { x: 1.5, y: 2.5 });
        Object.assign(t, { x: "str" + i });
        if (t.x === "str" + i && t.y === 2.5) genOk++;
    }

    // 5. Overflow slots (> kOverflowSlot on both source and target: start from `{}`).
    const dynBig = {};
    for (let k = 0; k < 19; k++) dynBig["p" + k] = k;
    dynBig.p19 = { v: 19 };
    let bigSum = 0;
    for (let i = 0; i < 80; i++) {
        const c = { ...dynBig };
        const tAssign = {};
        Object.assign(tAssign, dynBig);
        const { p0, p5, p10, ...r } = dynBig;
        bigSum += c.p0 + tAssign.p18 + c.p19.v + p0 + p5 + p10 + Object.keys(r).length + r.p19.v;
    }

    return total + "|" + emptyKeys + "|" + compSum + "|" + oldOk + "|" + genOk + "|" + bigSum;
}

function testMathMinMax() {
    function runMin(x, y) { return Math.min(x, y); }
    function runMax(x, y) { return Math.max(x, y); }

    const pairs = [
        [3, 5], [5, 3], [-10, -2], [-2, -2], [1.25, 1.25],
        [-0, +0], [+0, -0], [-0, -0], [+0, +0],
        [NaN, 1], [1, NaN], [NaN, NaN], [-0, NaN], [NaN, +0],
        [-Infinity, 0], [Infinity, 0], [Infinity, -Infinity], [-Infinity, NaN],
    ];

    let mins = "";
    let maxs = "";
    for (let r = 0; r < 100; r++) {
        const m0 = [];
        const m1 = [];
        for (let i = 0; i < pairs.length; i++) {
            const a = pairs[i][0];
            const b = pairs[i][1];
            const mn = runMin(a, b);
            const mx = runMax(a, b);
            m0.push(Object.is(mn, -0) ? "-0" : String(mn));
            m1.push(Object.is(mx, -0) ? "-0" : String(mx));
        }
        if (r === 99) {
            mins = m0.join(",");
            maxs = m1.join(",");
        }
    }
    // Arithmetic inputs (triggers speculative f64 lowering rather than array loads).
    let arithSum = 0;
    let negZeroHits = 0;
    let posZeroHits = 0;
    for (let i = 0; i < 200; i++) {
        const x = (i - 50) * 0.25;
        const y = (50 - i) * 0.25;
        arithSum += Math.min(x, y) + Math.max(x, y) * 2;
        const nz = -0 * (i + 1);
        const pz = 0 * (i + 1);
        if (Object.is(Math.min(nz, pz), -0) && Object.is(Math.min(pz, nz), -0)) negZeroHits++;
        if (Object.is(Math.max(nz, pz), 0) && Object.is(Math.max(pz, nz), 0)) posZeroHits++;
    }

    return mins + "|" + maxs + "|" + arithSum + ":" + negZeroHits + ":" + posZeroHits;
}

console.log("io:", testInstanceofHardened());
console.log("trunc:", testTruncate());
console.log("licm:", testLicmDenseGrowth());
console.log("spread:", testSpreadAndRest());
console.log("minmax:", testMathMinMax());
