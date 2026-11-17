// instanceof sites whose cell certifies a constructor, and every way the
// certificate must stop holding.
class A { constructor() { this.a = 1; } }
class B extends A {}
class C extends B {}
class D {}
function check(o, F) { return o instanceof F; }
const objs = [new A(), new B(), new C(), new D(), {}, Object.create(null), [], function () {}, 1, 'a', null, undefined];
function row(F) {
  let s = '';
  for (let k = 0; k < 3; k++) for (const o of objs) s += check(o, F) ? '1' : '0';
  return s;
}
console.log('A', row(A));
console.log('B', row(B));
console.log('C', row(C));
console.log('D', row(D));
console.log('Object', row(Object));
console.log('Function', row(Function));
console.log('Array', row(Array));

// A prototype reassigned after the site latched.
function P() {}
const p1 = new P();
for (let i = 0; i < 5; i++) check(p1, P);
const oldProto = P.prototype;
P.prototype = { replaced: true };
console.log('reassigned', check(p1, P), check(new P(), P), check(Object.create(oldProto), P));

// Symbol.hasInstance defined on the class itself after latching.
class H {}
const h = new H();
for (let i = 0; i < 5; i++) check(h, H);
Object.defineProperty(H, Symbol.hasInstance, { value: (v) => v === 42 });
console.log('own hasInstance', check(h, H), check(42, H));

// On a base class after a derived class's site latched.
class Base {}
class Derived extends Base {}
const d = new Derived();
for (let i = 0; i < 5; i++) check(d, Derived);
Object.defineProperty(Base, Symbol.hasInstance, { value: () => false });
console.log('inherited hasInstance', check(d, Derived), check(d, Base));

// A static hasInstance from the start.
class S { static [Symbol.hasInstance](v) { return typeof v === 'string'; } }
console.log('static', check('x', S), check(new S(), S));

// Bound functions and proxies on the right.
class T {}
const t = new T();
const boundT = T.bind(null);
console.log('bound', check(t, boundT), check({}, boundT));
const proxyT = new Proxy(T, {});
console.log('proxy', check(t, proxyT), check({}, proxyT));
function W() {}
const w = new W();
const lying = new Proxy(W, { get(target, key) { return key === 'prototype' ? Object.prototype : Reflect.get(target, key); } });
console.log('proxy prototype', check({}, lying), check(w, lying), check(Object.create(null), lying));

// The receiver's chain changed after latching.
class U {}
const u = new U();
for (let i = 0; i < 5; i++) check(u, U);
Object.setPrototypeOf(u, null);
console.log('setPrototypeOf null', check(u, U));
const v = {};
Object.setPrototypeOf(v, U.prototype);
console.log('setPrototypeOf', check(v, U));
Object.setPrototypeOf(U.prototype, D.prototype);
console.log('proto of prototype', check(new U(), D), check(new U(), U));

// A chain longer than the stub walks.
let F0 = class {};
let cur = F0;
for (let i = 0; i < 12; i++) cur = class extends cur {};
const deep = new cur();
console.log('deep', check(deep, F0), check(deep, D), check(deep, cur));

// A proxy on the receiver's chain.
const viaProxy = Object.create(new Proxy(A.prototype, {}));
console.log('proxy chain', check(viaProxy, A), check(viaProxy, D));

// Not callable / not an object on the right.
for (const bad of [{}, 1, null]) {
  try { check({}, bad); console.log('no throw'); } catch (e) { console.log(e.constructor.name); }
}
// A bound function of a class with its own Symbol.hasInstance, and a
// primitive on the left of a bound function.
class Prim { static [Symbol.hasInstance](v) { return v === 7; } }
const boundPrim = Prim.bind(null);
console.log('bound hasInstance', check(7, boundPrim), check(8, boundPrim), check(new Prim(), boundPrim));
// The answer a site remembers for a receiver shape, and what must drop it.
class U2 {}
class D2 {}
const u2 = new U2();
let seenFalse = 0;
for (let i = 0; i < 5; i++) if (!check(u2, D2)) seenFalse++;
Object.setPrototypeOf(U2.prototype, D2.prototype);
console.log('remembered then relinked', seenFalse, check(u2, D2), check(new U2(), D2));
const dictObj = new U2();
for (let i = 0; i < 40; i++) dictObj['k' + i] = i;
for (let i = 0; i < 40; i += 2) delete dictObj['k' + i];
for (let i = 0; i < 5; i++) check(dictObj, U2);
Object.setPrototypeOf(dictObj, D.prototype);
console.log('dictionary receiver', check(dictObj, U2), check(dictObj, D));
const mid = Object.create(U2.prototype);
const leaf = Object.create(mid);
for (let i = 0; i < 5; i++) check(leaf, U2);
Object.setPrototypeOf(mid, null);
console.log('middle relinked', check(leaf, U2), check(leaf, D2));
