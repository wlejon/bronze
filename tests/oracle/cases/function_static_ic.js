// Reads of a FUNCTION receiver's statics through one warm site: own data,
// own static getters (`this` is the class read through, a subclass included),
// statics inherited through `extends`, and the keys a function answers ahead
// of its base classes' statics (name, length, prototype, constructor). Each
// reader is one site meeting plain objects and functions whose statics have
// the same keys, so an entry one filled is met by the other.

function readShared(o) { return o.shared; }
function readX(o) { return o.x; }
function readCall(o) { return typeof o.call; }
function readToString(o) { return o.toString === Object.prototype.toString; }
function readName(o) { return o.name; }
function readLength(o) { return o.length; }
function readProto(o) { return typeof o.prototype; }
function readCtor(o) { return o.constructor === Function; }
function readK(o) { return o.k; }

let made = 0;
class Point {
  static get shared() { made++; return this === Point ? 'point' : 'sub:' + this.tag; }
}
class Sub extends Point { static tag = 'S'; }
class Deep extends Sub { static tag = 'D'; }

const out = [];
for (let i = 0; i < 4; i++) {
  out.push(readShared(Point), readShared(Sub), readShared(Deep), readShared({ shared: 'plain' }));
}
console.log(out.join(','), made);

// A subclass that overrides the getter, then a redefinition of the base's.
class Over extends Point { static get shared() { return 'over'; } }
const seen = [];
for (let i = 0; i < 3; i++) seen.push(readShared(Over), readShared(Sub));
Object.defineProperty(Point, 'shared', { get() { return 'redefined:' + (this === Sub); }, configurable: true });
for (let i = 0; i < 3; i++) seen.push(readShared(Sub), readShared(Point));
Object.defineProperty(Point, 'shared', { value: 'data now', configurable: true, writable: true });
for (let i = 0; i < 3; i++) seen.push(readShared(Sub), readShared(Point));
console.log(seen.join(','));

// Own data statics on classes and plain functions, and a plain object of the
// same keys at the same site.
function F() {}
F.x = 'Fx';
class C { static x = 'Cx'; }
class D extends C {}
class E extends D { static y = 1; }
const xs = [];
for (let i = 0; i < 3; i++) xs.push(readX(F), readX(C), readX(D), readX(E), readX({ x: 'ox' }), readX(() => 0));
console.log(xs.join(','));
// A static added to an intermediate class shadows the base's for the classes below it.
D.x = 'Dx';
for (let i = 0; i < 2; i++) xs.push(readX(D), readX(E), readX(C));
delete D.x;
for (let i = 0; i < 2; i++) xs.push(readX(D), readX(E));
C.x = 'Cx2';
xs.push(readX(E));
delete C.x;
xs.push(readX(E), readX(D), readX(C));
console.log(xs.slice(18).join(','));

// The plain object `{x}` and F's statics share a shape: an absent or
// inherited answer for one is no answer for the other.
const callTypes = [];
for (let i = 0; i < 3; i++) {
  callTypes.push(readCall({ x: 1 }), readCall(F), readToString({ x: 1 }), readToString(F));
}
console.log(callTypes.join(','));

// Keys a function answers before its bases' statics.
class NamedBase {
  static get name() { return 'base-name'; }
  static length = 99;
  static constructor() { return 'static-ctor'; }
}
class NamedSub extends NamedBase {}
function* gen() {}
const keys = [];
for (let i = 0; i < 3; i++) {
  keys.push(readName(NamedBase), readName(NamedSub), readName(F), readName(gen));
  keys.push(readLength(NamedBase), readLength(NamedSub), readLength(F));
  keys.push(readProto(NamedSub), readProto(() => 0), readCtor(NamedSub), readCtor(F), readCtor(gen));
}
console.log(keys.join(','));

// Bound functions, proxies, arrows, natives at the same site.
function G() {}
G.k = 'Gk';
const bound = G.bind(null);
const prox = new Proxy(G, {});
const arrow = () => 0;
arrow.k = 'arrowk';
Array.k = 'Arrayk';
const ks = [];
for (let i = 0; i < 3; i++) ks.push(readK(G), readK(bound), readK(prox), readK(arrow), readK(Array), readK({ k: 'ok' }));
delete Array.k;
ks.push(readK(Array));
console.log(ks.join(','));

// A frozen class, a getter that throws, and a static setter-only accessor.
class Frozen { static x = 'fx'; static get shared() { return 'frozen-shared'; } }
Object.freeze(Frozen);
class Throws { static get shared() { throw new Error('getter threw'); } }
class SetOnly { static set shared(v) {} }
const misc = [];
for (let i = 0; i < 3; i++) {
  misc.push(readX(Frozen), readShared(Frozen), readShared(SetOnly));
  try { readShared(Throws); } catch (e) { misc.push(e.message); }
}
console.log(misc.join(','));

// A prototype swap of a class's constructor.
class P1 { static x = 'p1'; }
class P2 { static x = 'p2'; }
class Q extends P1 {}
const swaps = [];
for (let i = 0; i < 2; i++) swaps.push(readX(Q));
Object.setPrototypeOf(Q, P2);
for (let i = 0; i < 2; i++) swaps.push(readX(Q));
console.log(swaps.join(','));
