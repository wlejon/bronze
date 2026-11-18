// Symbol.dispose / Symbol.asyncDispose exist as well-known symbols, and
// Object.defineProperties / Object.create define symbol-keyed descriptors.
// The descriptions and keyFor are the spec's (and Chromium's): node 22's
// polyfill registers them as Symbol.for('nodejs.dispose').
console.log(typeof Symbol.dispose, typeof Symbol.asyncDispose);
console.log(Symbol.dispose.description, Symbol.asyncDispose.description);
console.log(Symbol.dispose === Symbol.dispose, Symbol.dispose !== Symbol.asyncDispose);
console.log(Symbol.keyFor(Symbol.dispose));

class Handle {
  constructor() { this.open = true; }
  [Symbol.dispose]() { this.open = false; }
  async [Symbol.asyncDispose]() { this.open = false; return 'closed'; }
}
const h = new Handle();
h[Symbol.dispose]();
console.log(h.open);
new Handle()[Symbol.asyncDispose]().then((r) => console.log('async', r));

const s = Symbol('m');
const o = {};
Object.defineProperties(o, { [s]: { value: function () { return 7; } }, a: { value: 1 } });
console.log(typeof o[s], o[s](), o.a);

class C {}
const t = Symbol('t');
Object.defineProperties(C.prototype, { [t]: { value() { return 3; } } });
console.log(new C()[t]());

const u = Symbol('u');
const p = Object.create({}, { [u]: { value: 9, enumerable: true } });
console.log(p[u], Object.getOwnPropertySymbols(p).length);

// A non-enumerable symbol key of the descriptors object is skipped.
const hidden = Symbol('h');
const descs = { [s]: { value: 1 } };
Object.defineProperty(descs, hidden, { value: { value: 2 }, enumerable: false });
const q = Object.defineProperties({}, descs);
console.log(q[s], q[hidden]);

// Strings first, then symbols, in definition order.
const order = [];
const d2 = {};
d2[Symbol('z')] = { get() { order.push('sym'); return 0; } };
d2.b = { get() { order.push('b'); return 0; } };
Object.defineProperties({}, d2);
const r = Object.defineProperties({}, d2);
console.log(Reflect.ownKeys(r).map(String).join(','));
