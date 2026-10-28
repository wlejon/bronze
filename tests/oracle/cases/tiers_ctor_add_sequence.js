// A constructor's adds to `this`, each an inline transition store behind its
// own guard (il_ic_lowering.cpp). Each case below warms a constructor until it
// is optimized and then changes what its adds would meet: the program must see
// every add exactly as the generic store makes it.

const N = 6000;
function describe(o) {
  return Object.keys(o).map(k => k + '=' + JSON.stringify(o[k])).join(',');
}
function warm(make) {
  let last;
  for (let i = 0; i < N; i++) last = make(i);
  return last;
}

// 1. A setter, then a read-only property, appearing on the prototype for a
// key in the middle of the run, after the constructor is hot.
class P {
  constructor(a, b) {
    this.a = a; this.b = b; this.c = 0; this.d = 1.5; this.e = true; this.f = null;
  }
}
console.log('plain', describe(warm(i => new P(i, i * 2))));
let setterSaw = [];
Object.defineProperty(P.prototype, 'c', {
  set(v) { setterSaw.push(v + ':' + Object.keys(this).join('')); },
  get() { return 'proto-c'; },
  configurable: true,
});
const withSetter = warm(i => new P(i, -i));
console.log('setter', describe(withSetter), withSetter.c, setterSaw.length, setterSaw[0]);
delete P.prototype.c;
console.log('setter gone', describe(warm(i => new P(i, 1))));
Object.defineProperty(P.prototype, 'e', { value: 'ro', writable: false, configurable: true });
let thrown = 0, msg = '';
for (let i = 0; i < 100; i++) {
  try { new P(i, i); } catch (err) { thrown++; msg = err.constructor.name; }
}
console.log('read-only', thrown, msg);
delete P.prototype.e;
console.log('read-only gone', describe(warm(i => new P(i, 2))));

// 2. An exception between two adds: what was added before it stays, nothing
// after it is.
let captured = null;
class Q {
  constructor(a, boom) {
    captured = this;
    this.a = a; this.b = 2; this.c = 3;
    if (boom) throw new Error('boom ' + a);
    this.d = 4; this.e = 5;
  }
}
warm(i => new Q(i, false));
try { new Q(7, true); } catch (err) { console.log('thrown', err.message, describe(captured)); }

// 3. A non-extensible or frozen `this` before the run.
class R {
  constructor(a, lock) {
    if (lock === 1) Object.preventExtensions(this);
    if (lock === 2) Object.freeze(this);
    this.a = a; this.b = 2; this.c = 3;
  }
}
warm(i => new R(i, 0));
for (const lock of [1, 2]) {
  try { new R(1, lock); console.log('lock', lock, 'no throw'); } catch (err) { console.log('lock', lock, err.constructor.name); }
}

// 4. A subclass: the base's adds run on a `this` of another shape, mixed
// with the base's own instances.
class S extends P {
  constructor(a) { super(a, 'sub'); this.g = [a]; this.h = 'h'; }
}
let mixed = '';
for (let i = 0; i < N; i++) {
  const o = (i & 1) ? new S(i) : new P(i, 'base');
  if (i >= N - 2) mixed += describe(o) + ' ' + (o instanceof S) + '; ';
}
console.log('mixed', mixed);

// 5. Reflect.construct with a new.target whose prototype is another, and a
// Proxy of the class.
function Other() {}
Other.prototype.tag = 'other';
const viaReflect = warm(i => Reflect.construct(P, [i, 'r'], Other));
console.log('reflect', describe(viaReflect), viaReflect.tag, Object.getPrototypeOf(viaReflect) === Other.prototype);
const PP = new Proxy(P, {});
console.log('proxy', describe(warm(i => new PP(i, 'p'))));

// 6. A getter reading a half-built `this`.
class T {
  constructor(a) { this.a = a; this.b = this.peek; this.c = 3; this.d = this.peek; }
  get peek() { return Object.keys(this).join('') + ':' + this.c; }
}
console.log('getter', describe(warm(i => new T(i))));

// 7. Past the inline slots and past the first overflow block, with object
// values (write barriers) among the numbers.
class Wide {
  constructor(i) {
    this.f0 = i; this.f1 = [i]; this.f2 = 2; this.f3 = 3; this.f4 = 4; this.f5 = { i }; this.f6 = 6;
    this.f7 = 7; this.f8 = 8; this.f9 = 9; this.f10 = 10; this.f11 = 11; this.f12 = 12; this.f13 = 13;
    this.f14 = 14; this.f15 = 15; this.f16 = 16; this.f17 = 17; this.f18 = 18; this.f19 = 19; this.f20 = 20;
    this.f21 = 21; this.f22 = 22; this.f23 = 23; this.f24 = 24; this.f25 = 25; this.f26 = 26; this.f27 = 27;
    this.f28 = 28; this.f29 = 29; this.f30 = 30; this.f31 = 31; this.f32 = 32; this.f33 = 33; this.f34 = 34;
    this.f35 = 35; this.f36 = [36, i]; this.f37 = 37; this.f38 = 'x' + i; this.f39 = 39;
  }
}
const keep = [];
for (let i = 0; i < N; i++) { const w = new Wide(i); if (i % 1000 === 0) keep.push(w); }
console.log('wide', keep.length, describe(keep[keep.length - 1]));

// 8. A key stored twice in one run, and a class field the constructor
// stores again.
class Twice {
  x = 'field';
  constructor(a) { this.y = a; this.x = a + 1; this.z = 0; this.y = -a; this.w = 1; }
}
console.log('twice', describe(warm(i => new Twice(i))));
