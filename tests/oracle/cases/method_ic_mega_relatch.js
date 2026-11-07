// A full method site (both ways latched) answers a third shape from the
// thread's (shape, key) table, and every 64th such answer goes to the helper,
// which latches the receiver into way 0 and moves way 0's resident to way 1.
// The site's ways then follow the shapes it meets most, whichever came first:
// three.js's scene-graph walk met a light and a mesh first and sent every Bone
// through the table for good. Every answer must stay the receiver's own method
// across those relatches: a dominant shape arriving after two rare ones, a
// recursive tree walk, a prototype method patched mid-run, an own property
// shadowing the inherited method, closures over young environments latched
// and displaced with collections in between, and a receiver that is no plain
// object reaching the full site.
class Node {
    constructor(k) { this.k = k; this.children = []; this.n = 0; }
    tick(a) { this.n += a; return this.k.length + a; }
    walk(a) {
        let h = this.tick(a);
        for (let i = 0; i < this.children.length; i++) h = (h * 7 + this.children[i].walk(a)) | 0;
        return h;
    }
}
class Light extends Node { constructor() { super("light"); this.intensity = 1; } }
class Mesh extends Node { constructor() { super("mesh"); this.geometry = 1; this.material = 2; } }
class Bone extends Node { constructor(i) { super("bone" + i); this.isBone = true; } tick(a) { this.n += 2 * a; return this.n & 1023; } }
class Scene extends Node { constructor() { super("scene"); this.background = null; } }

function call(o, a) { return o.tick(a); }

function run(label, objs, reps) {
    let h = 0;
    for (let r = 0; r < reps; r++) for (const o of objs) h = (h * 31 + call(o, r)) | 0;
    console.log(label, h);
}

// Two rare shapes fill the site, then a dominant third shape: its first 63
// table answers stay in the stub, the 64th relatches, and the rare shapes
// keep arriving so the counter keeps crossing.
const light = new Light(), mesh = new Mesh(), scene = new Scene();
const bones = [];
for (let i = 0; i < 12; i++) bones.push(new Bone(i));
run("fill", [light, mesh], 3);
run("dominant", bones, 40);
run("mixed", [light, mesh, ...bones, scene, ...bones], 60);

// A recursive walk over a tree mixing the four classes, as updateMatrixWorld.
const root = new Scene();
root.children.push(new Light(), new Light());
for (let m = 0; m < 20; m++) {
    const me = new Mesh();
    let parent = me;
    for (let b = 0; b < 6; b++) {
        const bone = new Bone(b);
        parent.children.push(bone);
        if (b % 2 === 1) parent = bone;
    }
    root.children.push(me);
}
let wh = 0;
for (let f = 0; f < 50; f++) wh = (wh * 3 + root.walk(f)) | 0;
console.log("walk", wh);

// A prototype method patched while the dominant shape is answered from the
// table and while it sits in a way.
Bone.prototype.tick = function (a) { return 5000 + a; };
run("patchBone", [light, ...bones, mesh, ...bones], 30);
Node.prototype.tick = function (a) { return 6000 + a + this.k.length; };
run("patchNode", [light, mesh, scene, ...bones], 30);

// An own property shadowing the inherited method on one dominant instance.
bones[3].tick = function (a) { return 7000 + a; };
run("own", [light, mesh, ...bones], 30);

// Closures over fresh environments: the env words latched by a relatch are
// young, and garbage between the rounds promotes them while the ways trade.
function make(tag) {
    const salt = [tag, tag.length];
    return class { constructor() { this.t = tag; } tick(a) { return salt[1] * 100 + a; } };
}
const keep = [];
for (let round = 0; round < 8; round++) {
    const A = make("a" + round), B = make("bb" + round), C = make("ccc" + round);
    const a = new A(), b = new B(), cs = [];
    for (let i = 0; i < 16; i++) cs.push(new C());
    run("env" + round, [a, b, ...cs, a, ...cs], 12);
    const junk = [];
    for (let i = 0; i < 20000; i++) junk.push({ i, s: "x" + i });
    keep.push(junk.length);
    run("envafter" + round, [...cs, b, a, ...cs], 12);
}
console.log("kept", keep.length);

// Receivers that are no plain object at a full site: an array with an own
// `tick`, a function with a static one, and a primitive's prototype method.
const arr = [1, 2, 3];
arr.tick = function (a) { return this.length + a; };
function F() {}
F.tick = function (a) { return 900 + a; };
Number.prototype.tick = function (a) { return this + a; };
run("exotic", [light, mesh, ...bones, arr, F, 42, ...bones], 20);
