function testWriteIcPromotion() {
    "use strict";
    function writeVal(o, v) {
        o.val = v;
    }

    // Way 0 takes cold shape A, way 1 gets hot shape B (>120 writes), way 2 gets shape C.
    const a = { a0: 1, val: 0 };
    writeVal(a, 10);

    let sumB = 0;
    for (let i = 0; i < 160; i++) {
        const b = { b0: i, b1: i + 1, val: 0 };
        writeVal(b, i * 2);
        sumB += b.val;
    }

    // Also drive way 2 hot (>120 writes) with another site.
    function writeNum(o, n) {
        o.x = n;
    }
    const n0 = { k0: 1, x: 1.5 };
    const n1 = { k1: 1, x: 2.5 };
    const n2 = { k2: 1, x: 3.5 };
    writeNum(n0, 1.25);
    writeNum(n1, 2.25);
    for (let i = 0; i < 150; i++) {
        writeNum(n2, i + 0.5);
    }

    // Generalize double slot -> string after promotion.
    writeNum(n2, "generalized");

    // Overflow (>4 inline slots on `{}`) + object-slot + accessor + frozen rejection.
    function setLast(o, v) {
        o.p5 = v;
    }
    const ov0 = {};
    ov0.p0 = 0; ov0.p1 = 1; ov0.p2 = 2; ov0.p3 = 3; ov0.p4 = 4; ov0.p5 = 5;
    const ov1 = {};
    ov1.q0 = 0; ov1.p0 = 0; ov1.p1 = 1; ov1.p2 = 2; ov1.p3 = 3; ov1.p4 = 4; ov1.p5 = 5;
    setLast(ov0, 11);
    for (let i = 0; i < 140; i++) setLast(ov1, i);

    function setChild(o, c) {
        o.child = c;
    }
    const objA = { tag: "A", child: { id: 0 } };
    const objB = { tag: "B", extra: 1, child: { id: 0 } };
    setChild(objA, { id: 1 });
    for (let i = 0; i < 140; i++) setChild(objB, { id: i });
    setChild(objB, "non-plain");

    let accLog = 0;
    const accA = { get p5() { return 0; }, set p5(v) { accLog += v; } };
    const accProto = { get val() { return 1; }, set val(v) { accLog += v * 10; } };
    const accB = Object.create(accProto);
    writeVal(accA, 1);
    for (let i = 0; i < 130; i++) writeVal(accB, 1);
    setLast(accA, 7);

    const frz = Object.freeze({ val: 99 });
    let threw = false;
    try {
        writeVal(frz, 123);
    } catch (e) {
        threw = e instanceof TypeError;
    }

    return [
        a.val,
        sumB,
        n0.x,
        n1.x,
        n2.x,
        ov0.p5,
        ov1.p5,
        objA.child.id,
        objB.child,
        accLog,
        threw,
        frz.val,
    ].join("|");
}

function testElemStub64AndKeys() {
    function dynGet(o, k) {
        return o[k];
    }

    const base = { inheritedProp: 77 };
    const rec = Object.create(base);
    rec.alpha = 1;
    rec.beta = 2;
    rec["0"] = 100;
    rec["1"] = 101;
    rec["42"] = 142;
    rec["3.5"] = 350;
    rec["true"] = 500;
    rec["false"] = 501;

    // 50+ distinct keys on one shape to force hash collisions + beside_hits promotion.
    const big = {};
    const keys = [];
    for (let i = 0; i < 64; i++) {
        const k = "key_" + i;
        keys.push(k);
        big[k] = i * 3 + 1;
    }

    let acc = 0;
    for (let r = 0; r < 100; r++) {
        const dynKey = "alph" + "a"; // dynamically created heap string under GC
        acc += dynGet(rec, "alpha");
        acc += dynGet(rec, dynKey);
        acc += dynGet(rec, "beta");
        acc += dynGet(rec, "inheritedProp");
        acc += dynGet(rec, 0) + dynGet(rec, -0) + dynGet(rec, 1) + dynGet(rec, 42) + dynGet(rec, 3.5);
        acc += dynGet(rec, true) + dynGet(rec, false);
        const miss = dynGet(rec, "absentKey");
        if (miss !== undefined) acc += 1000000;
        for (let i = 0; i < keys.length; i++) {
            acc += dynGet(big, keys[i]);
        }
        // Allocation churn to trigger minor collections & ident sweeps mid-loop.
        if ((r & 7) === 0) {
            for (let g = 0; g < 60; g++) {
                const junk = { s: "j_" + g, d: g * 0.5 };
                if (junk.d < 0) acc -= 1;
            }
        }
    }

    // Frozen dictionary object
    const frozenDict = { k0: 10, k1: 20, k2: 30 };
    delete frozenDict.k1;
    Object.freeze(frozenDict);
    let frozenSum = 0;
    for (let i = 0; i < 90; i++) {
        frozenSum += dynGet(frozenDict, "k0") + dynGet(frozenDict, "k2");
        if (dynGet(frozenDict, "k1") !== undefined) frozenSum += 9999;
    }

    // Mutable dictionary object + delete invalidation
    const mutDict = { x: 5, y: 6, z: 7 };
    delete mutDict.z;
    let mutSum = 0;
    for (let i = 0; i < 90; i++) {
        mutSum += dynGet(mutDict, "x") + (dynGet(mutDict, "y") || 0);
        if (i === 45) delete mutDict.y;
    }

    return [acc, frozenSum, mutSum].join("|");
}

function testMinorGcAndArrayGrowth() {
    // Mix of Leaf objects (strings, NumberBlocks, typed-array buffers) and mixed Cell plain objects.
    let survivors = [];
    const tenuredArr = [1, 2, 3];
    const tenuredMixed = [{ k: 0 }];

    // Push enough allocation churn to promote `tenuredArr` and `tenuredMixed` to old gen.
    for (let g = 0; g < 8000; g++) {
        const tmp = {
            a: g * 1.25,
            b: { inner: g },
            c: (g & 1) ? g : "s" + g,
            arr: [g, g + 1],
        };
        if (tmp.a < -1) survivors.push(tmp);
    }

    // Now grow both tenured arrays (old-gen block copy) and young arrays via push.
    for (let i = 0; i < 40; i++) {
        tenuredArr.push(i * 1.5);
        tenuredMixed.push({ k: i + 1 });
        const freshNums = [];
        const freshVals = [];
        for (let j = 0; j < 25; j++) {
            freshNums.push(j + i * 0.5);
            freshVals.push({ j, label: "lbl_" + (i + j) });
        }
        const f32 = new Float32Array(4);
        f32[0] = freshNums[24];
        const ab = new ArrayBuffer(16);
        const dv = new Uint8Array(ab);
        dv[0] = i & 255;
        survivors.push({ freshNums, freshVals, f32, dv, mixedObj: { d: 1.5, o: freshVals[0], s: "ok_" + i } });
        if (survivors.length > 24) survivors.shift();
    }

    let check = tenuredArr.length + tenuredMixed.length + tenuredArr[42] + tenuredMixed[40].k;
    for (let i = 0; i < survivors.length; i++) {
        check += survivors[i].f32[0] + survivors[i].dv[0] + survivors[i].mixedObj.d + survivors[i].mixedObj.o.j;
    }
    return check;
}

function testConcatTemplates() {
    const t0 = ``;
    const t1 = `${"only"}`;
    const nums = [0, -0, 9, 10, 99, 100, 123456, 2147483647, -1, -42, 1.5, NaN, Infinity];
    const parts = [];
    for (let i = 0; i < nums.length; i++) {
        const x = nums[i];
        parts.push(`${x}`);
        parts.push(`p=${i}:${x}|end`);
    }
    const n0 = 1, n1 = 23, n2 = 456, n3 = 7890, s0 = "ab", s1 = "cd", s2 = "ef", s3 = "gh", s4 = "ij";
    const ten = `${n0}_${s0}_${n1}_${s1}_${n2}_${s2}_${n3}_${s3}_${s4}_z`;
    const u16 = `${"héllo"}_${42}_${"🚀"}`;
    const longPiece = "x".repeat(300);
    const longRes = `${"pre_"}${123}_${longPiece}`;
    return [t0, t1, parts.join(","), ten, u16, longRes.length, longRes.slice(0, 12), longRes.slice(-4)].join("|");
}

function testDictKeyed() {
    "use strict";
    // Dictionary-mode object via delete
    const map1 = { init: 1 };
    delete map1.init;

    for (let i = 0; i < 1100; i += 100) map1[i] = i + 1;
    map1[-0] = 999; // overwrites key "0"
    map1[42] = 420;
    let sameSlot = map1["42"];
    map1["42"] = 421;
    let afterStrWrite = map1[42];

    for (let i = 1024; i <= 5000; i += 497) map1[i] = "L_" + i;
    map1["customKey"] = { v: 88 };

    // Large map exceeding shape dictionary threshold (200 keys)
    const map2 = {};
    for (let i = 0; i < 200; i++) {
        const key = i < 100 ? i : 2000 + i;
        map2[key] = key * 2;
    }

    // Delete & re-add
    delete map1[100];
    const deletedRead = map1[100];
    map1[100] = 777;

    // Inherited Object.prototype lookup ("toString")
    const hasToStrFn = typeof map1["toString"] === "function";

    // Non-writable & non-extensible refusals (strict & sloppy)
    const locked = { keep: 1 };
    delete locked.keep;
    locked[10] = 10;
    Object.defineProperty(locked, "20", { value: 20, writable: false, configurable: true, enumerable: true });
    Object.preventExtensions(locked);
    let strictNw = false;
    let strictNe = false;
    try {
        locked[20] = 99;
    } catch (e) {
        strictNw = e instanceof TypeError;
    }
    try {
        locked[30] = 99;
    } catch (e) {
        strictNe = e instanceof TypeError;
    }

    return [
        map1[0],
        sameSlot,
        afterStrWrite,
        map1[100],
        deletedRead === undefined,
        map1[1024],
        map1[5000],
        map1["customKey"].v,
        map2[50] + map2[2150],
        hasToStrFn,
        strictNw,
        strictNe,
        locked[20],
        locked[30] === undefined,
    ].join("|");
}

function sloppyRefusalTest() {
    const locked = { tmp: 1 };
    delete locked.tmp;
    Object.defineProperty(locked, "20", { value: 20, writable: false, configurable: true, enumerable: true });
    Object.preventExtensions(locked);
    locked[20] = 999;
    locked[30] = 888;
    return locked[20] + ":" + String(locked[30]);
}

console.log("w6_ic:", testWriteIcPromotion());
console.log("w6_elem:", testElemStub64AndKeys());
console.log("w6_gc:", testMinorGcAndArrayGrowth());
console.log("w6_tpl:", testConcatTemplates());
console.log("w6_dict:", testDictKeyed(), sloppyRefusalTest());
