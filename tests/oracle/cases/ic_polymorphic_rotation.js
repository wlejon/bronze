// One property site, many receiver SHAPES — the arrangement a single-entry
// cache cannot survive, and the one three.js's renderer is made of: a tree of
// meshes, groups and lights all asked the same question on one pass.
//
// The site holds four ways. Four shapes fit; a fifth rotates the least
// recently installed out, and every answer must still be the receiver's own.

// --- 1. five shapes at one site, the fifth in an OVERFLOW slot -------------
class S1 {
    constructor() {
        this.v = 1;
    }
}
class S2 {
    constructor() {
        this.a = 0;
        this.v = 2;
    }
}
class S3 {
    constructor() {
        this.a = 0;
        this.b = 0;
        this.v = 3;
    }
}
class S4 {
    constructor() {
        this.a = 0;
        this.b = 0;
        this.c = 0;
        this.v = 4;
    }
}
class S5 {
    constructor() {
        this.a = 0;
        this.b = 0;
        this.c = 0;
        this.d = 0;
        this.v = 5;
    }
}

// ONE site. Every call reads `.v` from here and nowhere else.
function readV(o) {
    return o.v;
}

const objs = [new S1(), new S2(), new S3(), new S4(), new S5()];
let sum = 0;
for (let i = 0; i < 500; i = i + 1) {
    for (let j = 0; j < 5; j = j + 1) sum = sum + readV(objs[j]);
}
console.log(sum);

// Four of the five, which fit without rotating.
let sum4 = 0;
for (let i = 0; i < 500; i = i + 1) {
    for (let j = 0; j < 4; j = j + 1) sum4 = sum4 + readV(objs[j]);
}
console.log(sum4);

// --- 2. present on some shapes, ABSENT from others -------------------------
// The three.js marker probe exactly: the same site answers from a slot for one
// shape and `undefined` for another, and the two answers must not leak into
// each other.
class Mesh {
    constructor() {
        this.isMesh = true;
    }
}
class Group {
    constructor() {
        this.n = 0;
    }
}
class Light {
    constructor() {
        this.n = 0;
        this.i = 0;
    }
}
function countMesh(o) {
    return o.isMesh ? 1 : 0;
}

const mixed = [new Mesh(), new Group(), new Light(), new Mesh(), new Group()];
let hits = 0;
for (let i = 0; i < 400; i = i + 1) {
    for (let j = 0; j < 5; j = j + 1) hits = hits + countMesh(mixed[j]);
}
console.log(hits);

// One shape's answer changes; the other two entries at the same site must be
// untouched by it. The epoch bump retires all of them, and they refill.
Group.prototype.isMesh = true;
let hits2 = 0;
for (let i = 0; i < 400; i = i + 1) {
    for (let j = 0; j < 5; j = j + 1) hits2 = hits2 + countMesh(mixed[j]);
}
console.log(hits2);

// --- 3. own hit, proto hit and absent at ONE site --------------------------
// Three different KINDS of entry in one site's ways, which is the case a cache
// that only ever held one kind at a time never had to get right.
class P {}
P.prototype.k = "proto";
class Q extends P {
    constructor() {
        super();
        this.k = "own";
    }
}
class Rz {}

function readK(o) {
    return o.k;
}

const zoo = [new P(), new Q(), new Rz()];
let a = "";
let b = "";
let c = "";
for (let i = 0; i < 300; i = i + 1) {
    a = readK(zoo[0]);
    b = readK(zoo[1]);
    c = readK(zoo[2]);
}
console.log(a);
console.log(b);
console.log(c);

// --- 4. more shapes than ways, cycled forever ------------------------------
// Eight shapes through a four-way site. Every read misses into the helper for
// most of them, which is the cost of a site this wide — and every answer is
// still the right one, which is the part that matters.
class W0 {
    constructor() {
        this.w = 0;
    }
}
class W1 {
    constructor() {
        this.p0 = 0;
        this.w = 1;
    }
}
class W2 {
    constructor() {
        this.p0 = 0;
        this.p1 = 0;
        this.w = 2;
    }
}
class W3 {
    constructor() {
        this.p0 = 0;
        this.p1 = 0;
        this.p2 = 0;
        this.w = 3;
    }
}
class W4 {
    constructor() {
        this.p0 = 0;
        this.p1 = 0;
        this.p2 = 0;
        this.p3 = 0;
        this.w = 4;
    }
}
class W5 {
    constructor() {
        this.p0 = 0;
        this.p1 = 0;
        this.p2 = 0;
        this.p3 = 0;
        this.p4 = 0;
        this.w = 5;
    }
}
class W6 {
    constructor() {
        this.p0 = 0;
        this.p1 = 0;
        this.p2 = 0;
        this.p3 = 0;
        this.p4 = 0;
        this.p5 = 0;
        this.w = 6;
    }
}
class W7 {
    constructor() {
        this.p0 = 0;
        this.p1 = 0;
        this.p2 = 0;
        this.p3 = 0;
        this.p4 = 0;
        this.p5 = 0;
        this.p6 = 0;
        this.w = 7;
    }
}

function readW(o) {
    return o.w;
}

const wide = [new W0(), new W1(), new W2(), new W3(), new W4(), new W5(), new W6(), new W7()];
let wsum = 0;
for (let i = 0; i < 300; i = i + 1) {
    for (let j = 0; j < 8; j = j + 1) wsum = wsum + readW(wide[j]);
}
console.log(wsum);

// Backwards, so the rotation order is the other one.
let wrev = 0;
for (let i = 0; i < 300; i = i + 1) {
    for (let j = 7; j >= 0; j = j - 1) wrev = wrev + readW(wide[j]);
}
console.log(wrev);

// --- 5. a polymorphic site whose shapes are created DURING the loop --------
// Fresh shapes and a collecting allocation between every read, for the
// GC-stress re-run.
class Fresh {
    constructor(n) {
        this.n = n;
    }
}
let fsum = 0;
for (let i = 0; i < 400; i = i + 1) {
    const o = new Fresh(i);
    const junk = [i, i + 1, i + 2];
    fsum = fsum + o.n + junk[2];
    if (o.notThere !== undefined) fsum = fsum + 100000;
}
console.log(fsum);
