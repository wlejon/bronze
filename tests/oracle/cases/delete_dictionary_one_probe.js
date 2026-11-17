'use strict';
// delete on dictionary-mode objects: the attributes and the removal from one
// probe, with every answer delete can give.
const d = {};
for (let i = 0; i < 40; i++) d['k' + i] = i;
for (let i = 0; i < 40; i += 3) delete d['k' + i];
console.log(Object.keys(d).length, delete d.k0, delete d.missing, 'k0' in d, d.k1);
Object.defineProperty(d, 'fixed', { value: 1, configurable: false, enumerable: true });
try { delete d.fixed; console.log('no throw'); } catch (e) { console.log('strict', e.constructor.name, d.fixed); }
console.log((function () { return Reflect.deleteProperty(d, 'fixed'); })(), d.fixed);
Object.defineProperty(d, 'acc', { get() { return 'got'; }, set(v) {}, configurable: true, enumerable: true });
console.log(d.acc, delete d.acc, d.acc, 'acc' in d);
Object.defineProperty(d, 'acc', { get() { return 'again'; }, configurable: true });
console.log(d.acc);
d.k1 = 'readded';
console.log(Object.keys(d).slice(-3).join(','), d.k1);
const sym = Symbol('s');
d[sym] = 5;
console.log(delete d[sym], d[sym], Object.getOwnPropertySymbols(d).length);

// A dictionary used as a prototype: a delete must reach readers that cached
// the inherited property.
const proto = {};
for (let i = 0; i < 40; i++) proto['p' + i] = i;
for (let i = 0; i < 40; i += 2) delete proto['p' + i];
proto.shared = 'from proto';
const child = Object.create(proto);
function read(o) { return o.shared; }
let s = '';
for (let i = 0; i < 5; i++) s += read(child) + ';';
delete proto.shared;
console.log(s, read(child), 'shared' in child);
const grand = { shared: 'from grand' };
Object.setPrototypeOf(proto, grand);
console.log(read(child));
delete grand.shared;
console.log(read(child));

// Computed keys, numeric-looking names, and a frozen dictionary.
const m = {};
for (let i = 0; i < 50; i++) m['key' + i] = i;
for (let i = 0; i < 50; i++) if (i % 5) delete m['key' + i];
m['10'] = 'ten';
console.log(Object.keys(m).join(','), delete m[10], delete m['key0'], Object.keys(m).length);
Object.freeze(m);
try { delete m.key5; console.log('no throw'); } catch (e) { console.log('frozen', e.constructor.name, m.key5); }
