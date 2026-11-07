// Megamorphic property sites: more receiver shapes than a site has ways, so
// answers come from the thread's (shape, key) table and the read stub that
// moves a table hit to way 0 rewrites the site's way 0 and its packed row
// (bronze_abi_layout.h, "the HOT array") together. Each answer must be the
// receiver's own: own slots at different positions, a key on the prototype
// for some shapes and own for others, a getter, an absent key, and shapes
// that stop arriving while new ones take their place.
function mk(i) {
    const o = {};
    for (let j = 0; j < i % 7; j++) o["p" + j] = j;
    o.v = i;
    if (i % 3 === 0) o.w = i * 2;
    return o;
}
class Base { get g() { return 100; } }
Base.prototype.w = -1;
function mkProto(i) {
    const o = Object.create(Base.prototype);
    for (let j = 0; j < i % 5; j++) o["q" + j] = j;
    o.v = i + 1000;
    return o;
}

function getV(o) { return o.v; }
function getW(o) { return o.w; }
function getG(o) { return o.g; }
function getNone(o) { return o.nothere; }

const objs = [];
for (let i = 0; i < 40; i++) objs.push(i % 2 ? mk(i) : mkProto(i));

let h = 0;
for (let r = 0; r < 60; r++) {
    for (let i = 0; i < objs.length; i++) {
        const o = objs[(i * 7 + r * 3) % objs.length];
        const w = getW(o);
        const g = getG(o);
        h = (h * 31 + getV(o) + (w === undefined ? 5 : w) + (g === undefined ? 3 : g) + (getNone(o) === undefined ? 1 : 0)) | 0;
    }
}
console.log("mega", h);

// Phases: one set of shapes dominates, then another replaces it.
const phaseA = [], phaseB = [];
for (let i = 0; i < 6; i++) { phaseA.push(mk(i)); phaseB.push(mkProto(i + 6)); }
let p = 0;
for (let r = 0; r < 300; r++) {
    const xs = r < 150 ? phaseA : phaseB;
    for (const o of xs) p = (p * 17 + getV(o) + (getW(o) | 0)) | 0;
}
console.log("phases", p);

// A shape that was resident gains a property on its prototype mid-run.
const plain = [];
for (let i = 0; i < 8; i++) plain.push(mkProto(i));
let q = 0;
for (let r = 0; r < 40; r++) {
    if (r === 20) Base.prototype.nothere = 77;
    for (const o of plain) q = (q * 13 + (getNone(o) | 0) + getV(o)) | 0;
}
console.log("proto-add", q);
