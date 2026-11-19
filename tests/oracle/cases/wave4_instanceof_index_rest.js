class CustomBase {
    constructor(x) { this.x = x; }
}
class CustomSub extends CustomBase {
    constructor(x, y) { super(x); this.y = y; }
}
class SubArray extends Array {}
class SubF32 extends Float32Array {}

function checkAll(v) {
    return (
        (v instanceof Number ? 1 : 0) +
        (v instanceof String ? 2 : 0) +
        (v instanceof Boolean ? 4 : 0) +
        (v instanceof Float32Array ? 8 : 0) +
        (v instanceof Uint8Array ? 16 : 0) +
        (v instanceof Uint8ClampedArray ? 32 : 0) +
        (v instanceof Int32Array ? 64 : 0) +
        (v instanceof Map ? 128 : 0) +
        (v instanceof Set ? 256 : 0) +
        (v instanceof RegExp ? 512 : 0) +
        (v instanceof Error ? 1024 : 0) +
        (v instanceof Array ? 2048 : 0) +
        (v instanceof Object ? 4096 : 0) +
        (v instanceof Function ? 8192 : 0) +
        (v instanceof CustomBase ? 16384 : 0)
    );
}

function testInstanceof() {
    const samples = [
        0, -0, 3.5, "abc", true, null, undefined,
        new Number(7), new String("hi"), new Boolean(false),
        new Float32Array(2), new SubF32(2), new Uint8Array(2), new Uint8ClampedArray(2), new Int32Array(2),
        new Map(), new Set(), /ab+c/i, new Error("e"),
        [1, 2], new SubArray(2), { a: 1 }, () => 1,
        new CustomBase(1), new CustomSub(1, 2),
    ];
    let warm = 0;
    for (let r = 0; r < 60; r++) {
        for (let i = 0; i < samples.length; i++) warm += checkAll(samples[i]);
    }
    const perSample = samples.map(checkAll).join(",");

    // Typed and bare-array hot sites
    function probeArrAndTyped(x) {
        return (x instanceof Float32Array ? "f" : "-") + (x instanceof Uint8Array ? "u" : "-") + (x instanceof CustomBase ? "c" : "-");
    }
    let hot = "";
    for (let r = 0; r < 80; r++) {
        hot = probeArrAndTyped(samples[10]) + probeArrAndTyped(samples[12]) + probeArrAndTyped(samples[19]) + probeArrAndTyped(samples[24]);
    }

    // Mutate C.prototype and Symbol.hasInstance after warmup to test invalidation
    function Dyn() {}
    const inst = new Dyn();
    const isDyn = (o) => o instanceof Dyn;
    let beforeMut = "";
    for (let i = 0; i < 60; i++) beforeMut = String(isDyn(inst)) + ":" + String(isDyn([1])) + ":" + String(isDyn(new Float32Array(1)));
    Dyn.prototype = {};
    const afterProtoSwap = String(isDyn(inst)) + ":" + String(isDyn(new Dyn()));
    Object.defineProperty(CustomBase, Symbol.hasInstance, {
        value(o) { return typeof o === "number" && o > 10; },
        configurable: true,
    });
    const hasInstOverride = checkAll(20) + ":" + checkAll(new CustomSub(1, 2));
    return warm + "|" + perSample + "|" + hot + "|" + beforeMut + "|" + afterProtoSwap + "|" + hasInstOverride;
}

function fillStridedUvs(uvs, uvsOffset, uvsStride, count, indices, indicesOffset) {
    let i = 0;
    uvsOffset *= uvsStride;
    while (i < count) {
        uvs[uvsOffset] = i * 0.5;
        uvs[uvsOffset + 1] = i + 0.25;
        uvsOffset += uvsStride;
        indices[indicesOffset++] = i * 2;
        indices[indicesOffset++] = i * 2 + 1;
        i++;
    }
    return uvs.length + ":" + indices.length + ":" + uvsOffset + ":" + indicesOffset;
}

function testStridedLoops() {
    let res = "";
    for (let r = 0; r < 80; r++) {
        const uvs = [];
        const indices = [];
        res = fillStridedUvs(uvs, 0, 2, 16, indices, 0);
    }
    const growUvs = [9, 9];
    const growIdx = [8];
    const r2 = fillStridedUvs(growUvs, 1, 2, 10, growIdx, 1);
    const f32 = new Float32Array(6);
    const r3 = fillStridedUvs(f32, 0.5, 2, 2, [], 0);
    const huge = [];
    const r4 = fillStridedUvs(huge, 0, 2147483647, 1, [], 0);
    return res + "|" + r2 + "|" + growUvs[21] + "|" + r3 + "|" + f32[2] + "|" + r4;
}

function collectRest(head, ...rest) {
    let sum = head;
    for (let i = 0; i < rest.length; i++) {
        const v = rest[i];
        sum += typeof v === "number" ? v : (v && v.n ? v.n : 1);
    }
    return rest.length + ":" + (Array.isArray(rest) ? 1 : 0) + ":" + sum;
}

function testRestArgs() {
    let acc = "";
    for (let i = 0; i < 200; i++) {
        const a0 = collectRest(1);
        const a1 = collectRest(1, i);
        const a4 = collectRest(1, i, i + 1, i + 2, 0.5);
        const a8 = collectRest(0, 1, 2, 3, 4, 5, 6, 7, 8);
        const mixed = collectRest(2, "x", { n: i & 7 }, null, undefined, true, [1, 2]);
        if (i === 199) acc = a0 + "|" + a1 + "|" + a4 + "|" + a8 + "|" + mixed;
    }
    const args40 = [0];
    for (let k = 1; k <= 40; k++) args40.push(k);
    const big = collectRest(...args40);
    return acc + "|" + big;
}

function testTemplateInts() {
    const vals = [0, -0, 1, 9, 10, 42, 123456, 2147483647, -1, -42, 1.25, NaN, Infinity, -Infinity];
    let last = "";
    for (let r = 0; r < 60; r++) {
        const out = [];
        for (let i = 0; i < vals.length; i++) {
            const x = vals[i];
            out.push(`k_${i}_${x}_end`);
        }
        last = out.join(",");
    }
    return last;
}

console.log("io:", testInstanceof());
console.log("idx:", testStridedLoops());
console.log("rest:", testRestArgs());
console.log("tmpl:", testTemplateInts());
