function growAppendPairs(out, n) {
    let idx = 0;
    for (let i = 0; i < n; i++) {
        out[idx] = i * 1.5 + 0.25;
        out[idx + 1] = i * 2 + 1;
        idx += 2;
    }
    return idx;
}

function growAppendDemote(out, n) {
    let idx = out.length;
    for (let i = 0; i < n; i++) {
        out[idx++] = (i & 3) === 2 ? { v: i } : i * 0.5;
    }
    return out.length;
}

function testGrowWriteDeopts() {
    const a = [10, 20];
    const results = [];
    // Warm up growing write at len
    for (let r = 0; r < 60; r++) {
        a.length = 0;
        const written = growAppendPairs(a, 12);
        if (r === 59) results.push(written + ':' + a[0] + ':' + a[23]);
    }
    // Overwrite-after-grow
    const b = new Array(24).fill(0);
    growAppendPairs(b, 12);
    results.push(b[1] + ':' + b[22]);

    // Mixed object demotion during growing writes
    const mixed = [];
    for (let r = 0; r < 50; r++) {
        mixed.length = 0;
        growAppendDemote(mixed, 10);
    }
    results.push(mixed.length + ':' + mixed[2].v + ':' + mixed[5]);

    // Sparse and negative writes on same helper pattern
    function writeAt(arr, idx, val) {
        arr[idx] = val;
    }
    const target = [];
    for (let i = 0; i < 60; i++) writeAt(target, i, i * 2);
    writeAt(target, target.length + 3, 999);
    writeAt(target, -1, 'neg');
    results.push(target.length + ':' + target[63] + ':' + target['-1']);
    return results.join('|');
}

function testI32vsF64Bounds() {
    function loopLT(limit) {
        let s = 0;
        for (let i = 0; i < limit; i++) s += i + 1;
        return s;
    }
    function loopLE(limit) {
        let s = 0;
        for (let i = 0; i <= limit; i++) s += i + 1;
        return s;
    }
    function loopGT(limit) {
        let s = 0;
        for (let i = 10; i > limit; i--) s += i;
        return s;
    }
    function loopGE(limit) {
        let s = 0;
        for (let i = 10; i >= limit; i--) s += i;
        return s;
    }

    let warm = 0;
    for (let k = 0; k < 80; k++) {
        warm += loopLT(5) + loopLE(5) + loopGT(5) + loopGE(5);
    }

    const limits = [0, -0, 5.5, 4.9, -0.5, 10.5, NaN, Infinity, -Infinity];
    const out = [warm];
    for (const lim of limits) {
        const safeLT = lim === Infinity ? loopLT(12.2) : loopLT(lim);
        const safeLE = lim === Infinity ? loopLE(12.2) : loopLE(lim);
        const safeGT = lim === -Infinity ? loopGT(-2.2) : loopGT(lim);
        const safeGE = lim === -Infinity ? loopGE(-2.2) : loopGE(lim);
        out.push(safeLT + ',' + safeLE + ',' + safeGT + ',' + safeGE);
    }
    return out.join(';');
}

class Particle {
    constructor(x, y, meta, vx, vy) {
        this.x = x;
        this.y = y;
        this.meta = meta;
        this.vx = vx;
        this.vy = vy;
    }
}

function testGcAndProtoSetter() {
    // Exercise NumberBlock, Plain double_slots, and incremental marking cycles
    const oldKeep = [];
    for (let iter = 0; iter < 12000; iter++) {
        const arr = new Array();
        arr[0] = iter * 0.5;
        arr[1] = iter * 1.5;
        const p = new Particle(iter * 0.25, iter * 0.75, { tag: 'p' + (iter & 7), arr: [iter, iter + 1] }, 1.5, -2.5);
        if ((iter & 31) === 0) {
            oldKeep.push({ arr, p, empty: [] });
        }
    }
    let acc = 0;
    for (let i = 0; i < oldKeep.length; i++) {
        const entry = oldKeep[i];
        entry.empty[0] = i * 2.5;
        acc += entry.arr[0] + entry.arr[1] + entry.p.x + entry.p.meta.arr[1] + entry.empty[0];
    }

    // Prototype index setter after warmup must trap via is.plain_array hazard check
    let intercepted = 0;
    function hotAppend(arr, i, v) {
        arr[i] = v;
    }
    const base = [];
    for (let i = 0; i < 80; i++) hotAppend(base, i, i);
    const proto = Object.create(Array.prototype);
    Object.defineProperty(proto, 0, {
        configurable: true,
        set(v) { intercepted += v; },
        get() { return -77; }
    });
    Object.prototype[4] = 'op4';
    const holeBare = [];
    hotAppend(holeBare, 0, 5);
    const inheritedAt4 = holeBare[4];
    delete Object.prototype[4];
    const trapped = [];
    Object.setPrototypeOf(trapped, proto);
    hotAppend(trapped, 0, 123);
    const readBack = trapped[0];
    return acc.toFixed(1) + ':' + intercepted + ':' + readBack + ':' + inheritedAt4;
}

console.log('grow', testGrowWriteDeopts());
console.log('bounds', testI32vsF64Bounds());
console.log('gcproto', testGcAndProtoSetter());
