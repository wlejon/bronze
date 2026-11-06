// A profile's inline way chains (il.h `icChain`) and method-site hints
// (kMethodPolyShapes, own-slot sites, dropped native-callee targets): each site
// warms up on the receivers a profile records, and then what the inline code
// read stops being true. Absent keys appear on the instance, on the
// prototype, on Object.prototype and as a getter; a prototype constant
// changes, moves to the instance, becomes a getter and the prototype is
// swapped; receivers change shape and grow past four shapes; methods are
// replaced on the prototype and on the instance; an own-slot callee changes
// closure; a typed array's `set` and `Array.isArray` are replaced. Whatever
// the build guessed, the bytes are these.

const out = [];
const log = (...xs) => out.push(xs.join(" "));

// Absent keys on four shapes: `m.isSprite` misses on every one.
class MatA { constructor() { this.a = 1; } }
class MatB { constructor() { this.b = 2; this.c = 3; } }
class MatC { constructor() { this.d = 4; } }
class MatD { constructor() { this.e = 5; this.f = 6; this.g = 7; } }
const mats = [new MatA(), new MatB(), new MatC(), new MatD()];
function isSprite(m) { return m.isSprite; }
function countSprites() {
  let n = 0, seen = "";
  for (let i = 0; i < 400; ++i) {
    const v = isSprite(mats[i & 3]);
    if (v !== undefined) { ++n; if (i < 4) seen += String(v) + ","; }
  }
  return n + ":" + seen;
}
log("absent warm", countSprites());
mats[1].isSprite = "own";
log("absent own", countSprites());
delete mats[1].isSprite;
log("absent deleted", countSprites());
MatC.prototype.isSprite = "proto";
log("absent proto", countSprites());
delete MatC.prototype.isSprite;
let getterCalls = 0;
Object.defineProperty(MatD.prototype, "isSprite", { get() { ++getterCalls; return "get" + this.e; }, configurable: true });
log("absent getter", countSprites(), getterCalls);
delete MatD.prototype.isSprite;
Object.prototype.isSprite = "root";
log("absent root", countSprites());
delete Object.prototype.isSprite;
log("absent again", countSprites());

// A constant one prototype link up, as WebGL2RenderingContext's are.
class Ctx { constructor() { this.canvas = null; } }
Ctx.prototype.CULL_FACE = 2884;
Ctx.prototype.DEPTH_TEST = 2929;
class Ctx2 { constructor() { this.canvas = 1; this.extra = 2; } }
Ctx2.prototype.CULL_FACE = 11;
Ctx2.prototype.DEPTH_TEST = 22;
const gl = new Ctx(), gl2 = new Ctx2();
function sumCaps(g) {
  let s = 0;
  for (let i = 0; i < 300; ++i) s += g.CULL_FACE + g.DEPTH_TEST;
  return s;
}
function both() { return sumCaps(gl) + "/" + sumCaps(gl2); }
log("proto warm", both());
Ctx.prototype.CULL_FACE = 1;
log("proto changed", both());
gl.DEPTH_TEST = 3;
log("proto shadowed", both());
delete gl.DEPTH_TEST;
Object.defineProperty(Ctx.prototype, "CULL_FACE", { get() { return 100; }, configurable: true });
log("proto getter", both());
Object.setPrototypeOf(gl, Ctx2.prototype);
log("proto swapped", both());
// The holder turned into a dictionary: many deletes on the prototype.
for (let k = 0; k < 40; ++k) Ctx2.prototype["tmp" + k] = k;
for (let k = 0; k < 40; ++k) delete Ctx2.prototype["tmp" + k];
Ctx2.prototype.DEPTH_TEST = 23;
log("proto dict", both());

// Own data slots on two to four shapes, then more.
function mk(i) {
  switch (i % 7) {
    case 0: return { fog: 1, w: 0 };
    case 1: return { w: 2, fog: 3 };
    case 2: return { x: 0, w: 4, fog: 5 };
    case 3: return { y: 0, z: 0, fog: 7, w: 6 };
    case 4: return { q: 1, fog: 9 };
    case 5: return { fog: 11, r: 2, s: 3 };
    default: return Object.assign(Object.create({ fog: 13 }), { t: 1 });
  }
}
function readFog(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; ++i) { const v = objs[i].fog; s += v === undefined ? 1000 : v; }
  return s;
}
const four = [];
for (let i = 0; i < 200; ++i) four.push(mk(i % 4));
log("poly warm", readFog(four));
four[5].fog = 100;
four[6].newKey = 1;
delete four[7].fog;
log("poly changed", String(readFog(four)));
const seven = [];
for (let i = 0; i < 210; ++i) seven.push(mk(i));
log("poly seven", readFog(seven), readFog(four.slice(0, 8)));

// Methods: a prototype method on two shapes, replaced after warmup, then
// shadowed on one instance.
class Mat { constructor(k) { this.k = k; } onBefore(x) { return this.k + x; } }
class SubMat extends Mat { constructor(k) { super(k); this.extra = 1; } }
const ms = [new Mat(1), new SubMat(2), new Mat(3), new SubMat(4)];
function callAll() { let s = 0; for (let i = 0; i < 400; ++i) s += ms[i & 3].onBefore(i); return s; }
log("method warm", callAll());
Mat.prototype.onBefore = function (x) { return this.k * 2 - x; };
log("method replaced", callAll());
ms[1].onBefore = function (x) { return 7; };
log("method own", callAll());
SubMat.prototype.onBefore = function (x) { return -this.k; };
log("method sub", callAll());
delete ms[1].onBefore;
log("method deleted", callAll());

// Own-slot callees: a factory's literal, two closures in one shape.
function makeProps(tag) {
  const store = new Map();
  return {
    get(o) { let v = store.get(o); if (v === undefined) { v = tag + store.size; store.set(o, v); } return v; },
    dispose() { store.clear(); },
  };
}
const p1 = makeProps("a"), p2 = makeProps("b");
const keys = [{}, {}, {}];
function getAll() { let s = ""; for (let i = 0; i < 60; ++i) { const p = (i % 3) ? p1 : p2; s = p.get(keys[i % 3]); } return s + p1.get(keys[0]) + p2.get(keys[1]); }
log("slot warm", getAll());
p1.get = (o) => "swapped";
log("slot swapped", getAll());
p2.get = 5;
try { getAll(); } catch (e) { log("slot not callable", e instanceof TypeError); }
p2.get = function () { return "fn"; };
log("slot restored", getAll());

// A typed array's `set` and Array.isArray, replaced after warmup.
const dst = new Float32Array(16), src = [1, 2, 3, 4];
function setMany() { for (let i = 0; i < 100; ++i) dst.set(src, i & 7); return dst.join(","); }
log("set warm", setMany());
const realSet = Float32Array.prototype.set;
Float32Array.prototype.set = function (a, off) { this[off] = -1; };
log("set patched", setMany());
Float32Array.prototype.set = realSet;
dst.set = function () { this[15] = 9; };
log("set own", setMany());
function arrays(xs) { let n = 0; for (let i = 0; i < xs.length; ++i) if (Array.isArray(xs[i])) ++n; return n; }
const mixed = [];
for (let i = 0; i < 300; ++i) mixed.push(i % 3 ? [i] : { i });
log("isArray warm", arrays(mixed));
const realIsArray = Array.isArray;
Array.isArray = (x) => typeof x === "object";
log("isArray replaced", arrays(mixed));
Array.isArray = realIsArray;

// A push site whose receiver is nearly always an Array, sometimes a list.
class List { constructor() { this.items = []; } push(x) { this.items.push(x * 10); return this.items.length; } }
function fill(target, n) { let r = 0; for (let i = 0; i < n; ++i) r = target.push(i); return r; }
const arr = [];
log("push warm", fill(arr, 500), arr.length);
const list = new List();
log("push list", fill(list, 5), list.items.join(","));
arr.push = function (x) { return -x; };
log("push own", fill(arr, 3), arr.length);

console.log(out.join("\n"));
