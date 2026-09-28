// A method call's cached callee against every way a program can change what
// the prototype chain answers after the site has latched. `this.m()` inside a
// method (the site il2mir may call directly) and an outside `o.m()` both run
// hot enough to tier up before each change, then again after it: a callee
// cached on the receiver's shape alone keeps calling the old function, because
// none of these changes moves the receiver's shape.

function warm(f, n) {
  let acc = 0;
  for (let i = 0; i < n; i++) acc = f(i);
  return acc;
}
const N = 3000;

// ---- patching a method after warm-up, and before it ----------------------
{
  class Box {
    constructor(v) { this.v = v; }
    get() { return this.v; }
    twice() { return this.get() * 2; }
  }
  const b = new Box(3);
  warm(() => b.twice(), N);
  const orig = Box.prototype.get;
  Box.prototype.get = function () { return orig.call(this) + 100; };
  console.log('after warm-up:', b.get(), b.twice(), new Box(1).twice());
  warm(() => b.twice(), N);
  Box.prototype.get = orig;
  console.log('restored:', b.twice(), warm(() => b.twice(), N));

  class Early {
    k() { return 1; }
    call() { return this.k() + 10; }
  }
  Early.prototype.k = function () { return 2; };
  console.log('patched before warm-up:', warm(() => new Early().call(), N));
  Early.prototype.k = function () { return 3; };
  console.log('patched again:', new Early().call());
}

// ---- an inherited method, patched on the base class ----------------------
{
  class Base {
    name() { return 'base'; }
    describe() { return '<' + this.name() + '>'; }
  }
  class Mid extends Base {}
  class Leaf extends Mid {}
  const l = new Leaf();
  warm(() => l.describe(), N);
  Base.prototype.name = function () { return 'patched-base'; };
  console.log('base patched:', l.describe(), new Mid().describe());
  warm(() => l.describe(), N);
  // An add to an intermediate prototype shadows the base's method without
  // touching the receiver's shape.
  Mid.prototype.name = function () { return 'mid'; };
  console.log('mid shadows:', l.describe(), new Base().describe());
}

// ---- deleting an own method falls back to the base -----------------------
{
  class A {
    who() { return 'A'; }
    tell() { return this.who(); }
  }
  class B extends A {
    who() { return 'B'; }
  }
  const b = new B();
  warm(() => b.tell(), N);
  delete B.prototype.who;
  console.log('after delete:', b.tell(), warm(() => b.tell(), N));
  B.prototype.who = function () { return 'B again'; };
  console.log('re-added:', b.tell());
}

// ---- Object.setPrototypeOf ----------------------------------------------
{
  class P {
    tag() { return 'P'; }
    show() { return this.tag(); }
  }
  class Q {
    tag() { return 'Q'; }
    show() { return 'Q.show ' + this.tag(); }
  }
  class C extends P {}
  const c = new C();
  warm(() => c.show(), N);
  Object.setPrototypeOf(C.prototype, Q.prototype);
  console.log('prototype of prototype swapped:', c.show(), warm(() => c.show(), N));
  const lone = new P();
  warm(() => lone.show(), N);
  Object.setPrototypeOf(lone, Q.prototype);
  console.log('instance swapped:', lone.show());
}

// ---- Object.defineProperty: a getter, and a value -----------------------
{
  class D {
    m() { return 'm'; }
    run() { return this.m(); }
  }
  const d = new D();
  warm(() => d.run(), N);
  let made = 0;
  Object.defineProperty(D.prototype, 'm', {
    get() { made++; return () => 'from getter'; },
    configurable: true,
  });
  console.log('getter installed:', d.run(), warm(() => d.run(), N), made);
  Object.defineProperty(D.prototype, 'm', { value() { return 'defined value'; }, configurable: true });
  console.log('value defined:', d.run(), warm(() => d.run(), N));

  class E {
    m() { return 'e'; }
    run() { return this.m(); }
  }
  const e = new E();
  warm(() => e.run(), N);
  Object.defineProperty(E.prototype, 'm', { value: function () { return 'e2'; } });
  console.log('value over method:', e.run());
}

// ---- super.m() -----------------------------------------------------------
{
  class A {
    m() { return 'A'; }
  }
  class B extends A {
    m() { return 'B>' + super.m(); }
    outer() { return this.m(); }
  }
  const b = new B();
  warm(() => b.outer(), N);
  A.prototype.m = function () { return 'A2'; };
  console.log('super target patched:', b.outer(), warm(() => b.m(), N));
  B.prototype.m = function () { return 'B2'; };
  console.log('overrider patched:', b.outer());
}

// ---- static methods, own and inherited -----------------------------------
{
  class S {
    static helper() { return 1; }
    static run() { return this.helper() + 10; }
  }
  class T extends S {}
  warm(() => S.run() + T.run(), N);
  S.helper = function () { return 2; };
  console.log('static patched:', S.run(), T.run());
  warm(() => T.run(), N);
  T.helper = function () { return 3; };
  console.log('static shadowed on subclass:', S.run(), T.run());
  delete T.helper;
  console.log('static shadow deleted:', T.run());
}

// ---- getters and setters -------------------------------------------------
{
  class G {
    constructor() { this.store = 0; }
    get v() { return 1; }
    set w(x) { this.store = x; }
    total() { return this.v; }
    put(x) { this.w = x; return this.store; }
  }
  const g = new G();
  warm(() => g.total() + g.put(5), N);
  Object.defineProperty(G.prototype, 'v', { get() { return 2; }, configurable: true });
  Object.defineProperty(G.prototype, 'w', { set(x) { this.store = x * 100; }, configurable: true });
  console.log('accessors replaced:', g.total(), g.put(5), warm(() => g.total() + g.put(1), N));
  Object.defineProperty(G.prototype, 'v', { value: 7, configurable: true, writable: true });
  console.log('getter to data:', g.total());
}

// ---- an object that becomes a prototype late -----------------------------
{
  // `setGreet` is one assignment site. It runs hot on `base` while `base` is
  // an ordinary object, then again once `base` is a prototype.
  const base = { greet() { return 'hi'; }, other: 0 };
  function setGreet(o, f) { o.greet = f; }
  const hello = function () { return 'hello'; };
  for (let i = 0; i < N; i++) setGreet(base, hello);
  const child = Object.create(base);
  function callGreet(o) { return o.greet(); }
  warm(() => callGreet(child), N);
  setGreet(base, function () { return 'patched late'; });
  console.log('late prototype:', callGreet(child), warm(() => callGreet(child), N));
  // A same-shape stranger through the same site is still an ordinary write.
  const stranger = { greet() { return 's'; }, other: 0 };
  setGreet(stranger, hello);
  console.log('stranger:', stranger.greet(), callGreet(child));
}

// ---- computed keys, Object.assign, and a polymorphic site ----------------
{
  class K {
    a() { return 'a'; }
    b() { return 'b'; }
    both() { return this.a() + this.b(); }
  }
  const k = new K();
  warm(() => k.both(), N);
  const key = ['a', 'b'][warm((i) => i % 2, 3)];
  K.prototype[key] = function () { return 'A'; };
  console.log('computed key:', k.both());
  Object.assign(K.prototype, { b() { return 'B'; } });
  console.log('Object.assign:', k.both());

  class X { f() { return 'x'; } }
  class Y { f() { return 'y'; } }
  const xs = [new X(), new Y()];
  function callF(o) { return o.f(); }
  warm((i) => callF(xs[i & 1]), N);
  X.prototype.f = function () { return 'x2'; };
  Y.prototype.f = function () { return 'y2'; };
  console.log('polymorphic site:', callF(xs[0]), callF(xs[1]));
}

// ---- an own method on the instance shadows the class's -------------------
{
  class O {
    m() { return 'class'; }
    run() { return this.m(); }
  }
  const o = new O();
  warm(() => o.run(), N);
  o.m = function () { return 'own'; };
  console.log('own shadow:', o.run(), new O().run());
  o.m = function () { return 'own2'; };
  console.log('own replaced:', o.run(), warm(() => o.run(), N));
}
