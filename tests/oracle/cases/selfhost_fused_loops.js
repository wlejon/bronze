// The self-hosted Array loops after inlining: `k in O` fused with `O[k]`,
// LengthOfArrayLike read off the header, a reduce accumulator carried as a
// double behind one guard, and the C++ indexOf/includes walks. Each case
// changes what the fast path assumed part way through.
"use strict";

function show(label, v) {
    console.log(label, typeof v === "string" ? JSON.stringify(v) : String(v));
}

// 1. reduce: the accumulator stops being a Number mid-loop.
{
    const a = [1, 2, 3, "x", 5, 6];
    show("reduce mixed", a.reduce((s, x) => s + x, 0));
    show("reduce string seed", [1, 2, 3].reduce((s, x) => s + x, ""));
    show("reduce no seed", [4, 5, 6].reduce((s, x) => s + x));
    show("reduce objects", [{ v: 1 }, { v: 2 }].reduce((s, o) => s + o.v, 0));
    const big = [];
    for (let i = 0; i < 2000; i++) big.push(i * 0.5);
    let total = 0;
    for (let r = 0; r < 30; r++) total += big.reduce((s, x) => s + x, r);
    show("reduce hot", total);
    big[1500] = "s";
    show("reduce hot then string", big.reduce((s, x) => s + x, 0));
}

// 2. holes answered by the prototype chain, read through `in` + Get.
{
    const a = [1, , 3, , 5];
    show("map holes", JSON.stringify(a.map((x) => x * 2)));
    Object.prototype[1] = 100;
    show("map proto index", JSON.stringify(a.map((x) => x * 2)));
    show("reduce proto index", a.reduce((s, x) => s + x, 0));
    show("filter proto index", JSON.stringify(a.filter((x) => x > 2)));
    delete Object.prototype[1];
    Object.defineProperty(Object.prototype, 3, {
        get() { return 7; },
        configurable: true,
    });
    show("forEach proto getter", (() => { let s = 0; a.forEach((x) => { s += x; }); return s; })());
    show("some proto getter", a.some((x) => x === 7));
    delete Object.prototype[3];
    show("after delete", a.reduce((s, x) => s + x, 0));
}

// 3. a proxy on the chain answers `has` and `get` for the holes.
{
    const a = [10, , 30];
    const log = [];
    Object.setPrototypeOf(a, new Proxy(Array.prototype, {
        has(t, k) { log.push("has " + String(k)); return k === "1" || Reflect.has(t, k); },
        get(t, k, r) { if (k === "1") return 20; return Reflect.get(t, k, r); },
    }));
    show("proxy map", JSON.stringify(Array.prototype.map.call(a, (x) => x + 1)));
    show("proxy log", log.join(","));
}

// 4. the callback shrinks, grows, and punches holes in the array it walks.
{
    const a = [1, 2, 3, 4, 5, 6, 7, 8];
    show("shrink", JSON.stringify(a.map((x, i, o) => { if (i === 2) o.length = 4; return x; })));
    const b = [1, 2, 3];
    show("grow", b.reduce((s, x, i, o) => { if (i === 0) o.push(100); return s + x; }, 0));
    const c = [1, 2, 3, 4, 5];
    show("delete ahead", JSON.stringify(c.filter((x, i, o) => { if (i === 0) delete o[2]; return true; })));
    const d = [1, 2, 3, 4];
    show("kind change", JSON.stringify(d.map((x, i, o) => { if (i === 1) o[3] = "four"; return o[3]; })));
    const e = [1, 2, 3];
    show("zero length", e.reduce((s, x, i, o) => { o.length = 0; return s + x; }, 0));
    const f = [5, 6, 7];
    show("find after shrink", f.find((x, i, o) => { o.length = 1; return x === 7; }));
    const g = [1.5, 2.5, 3.5];
    show("frozen", Object.isFrozen(Object.freeze(g)) && g.reduce((s, x) => s + x, 0));
}

// 5. array-likes and the arguments object through the same builtins.
{
    const like = { length: 3, 0: 1, 2: 3 };
    show("like map", JSON.stringify(Array.prototype.map.call(like, (x) => x * 3)));
    show("like reduce", Array.prototype.reduce.call(like, (s, x) => s + x, 0));
    show("like length string", Array.prototype.reduce.call({ length: "2", 0: 1, 1: 2 }, (s, x) => s + x, 0));
    (function () {
        show("arguments reduce", Array.prototype.reduce.call(arguments, (s, x) => s + x, 0));
    })(1, 2, 3, 4);
    show("string map", JSON.stringify(Array.prototype.map.call("abc", (c) => c + c)));
}

// 6. indexOf / includes: NaN, -0, strings, BigInts, Int32 forms, fromIndex.
{
    const a = [0, -0, NaN, "1", 1, 2n, null, undefined, , {}];
    show("indexOf NaN", a.indexOf(NaN));
    show("includes NaN", a.includes(NaN));
    show("indexOf -0", a.indexOf(-0));
    show("indexOf 0", a.indexOf(0));
    show("indexOf str", a.indexOf("1"));
    show("indexOf num", a.indexOf(1));
    show("indexOf bigint", a.indexOf(2n));
    show("indexOf null", a.indexOf(null));
    show("indexOf undefined", a.indexOf(undefined));
    show("includes undefined (hole)", [1, , 3].includes(undefined));
    show("indexOf undefined (hole)", [1, , 3].indexOf(undefined));
    const bits = [];
    for (let i = 0; i < 8; i++) bits.push((i * 3) | 0);
    show("indexOf int32", bits.indexOf(9));
    show("indexOf int32 needle", bits.indexOf((3 << 1) | 0));
    const strs = ["a" + "b", "c", "ab"];
    show("indexOf built string", strs.indexOf("a" + "b"));
    let calls = 0;
    const empty = [];
    show("empty fromIndex", empty.indexOf(1, { valueOf() { calls++; return 0; } }));
    show("empty includes fromIndex", empty.includes(1, { valueOf() { calls++; return 0; } }));
    show("fromIndex calls", calls);
    const shrinking = [1, 2, 3, 4, 5];
    show("fromIndex shrinks", shrinking.indexOf(4, { valueOf() { shrinking.length = 2; return 0; } }));
    show("fromIndex negative", [1, 2, 3, 1].indexOf(1, -2));
    show("fromIndex big", [1, 2, 3].indexOf(1, 10));
    const obj = {};
    const objs = [{}, obj, {}];
    let hits = 0;
    for (let r = 0; r < 1000; r++) hits += objs.indexOf(obj);
    show("indexOf objects hot", hits);
    Object.prototype[1] = "p";
    show("indexOf proto hole", ["x", , "z"].indexOf("p"));
    show("includes proto hole", ["x", , "z"].includes("p"));
    delete Object.prototype[1];
    show("indexOf on like", Array.prototype.indexOf.call({ length: 2, 0: "a", 1: "b" }, "b"));
    try {
        Array.prototype.indexOf.call(null, 1);
    } catch (e) {
        show("indexOf null this", e instanceof TypeError);
    }
}

// 7. hand-written loops over `length`, with the array changing under them.
{
    function total(a) {
        let s = 0;
        for (let i = 0; i < a.length; i++) s += a[i];
        return s;
    }
    const a = [];
    for (let i = 0; i < 100; i++) a.push(i);
    let t = 0;
    for (let r = 0; r < 50; r++) t += total(a);
    show("hand sum", t);
    show("hand sum holes", total([1, , 3]));
    show("hand sum string", total([1, "2", 3]));
    function shrinkWalk(a) {
        let n = 0;
        for (let i = 0; i < a.length; i++) {
            if (i === 3) a.length = 5;
            n += a[i];
        }
        return n;
    }
    const b = [];
    for (let i = 0; i < 20; i++) b.push(1);
    show("hand shrink", shrinkWalk(b));
    function inc(a) {
        const out = [];
        for (let i = 0; i < a.length; i++) out.push(a[i] + 1);
        return out;
    }
    show("hand map", JSON.stringify(inc([1, 2, 3])));
    show("hand map like", JSON.stringify(inc({ length: 2, 0: 5, 1: 6 })));
    show("hand map typed", JSON.stringify(inc(new Float64Array([0.5, 1.5]))));
}
