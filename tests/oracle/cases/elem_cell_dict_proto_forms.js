// A computed read's site cell in its prototype and dictionary forms, and
// every change that must stop it answering: a key deleted from an id-keyed
// map and added back at another slot, a key redefined as a getter, a
// prototype method replaced or shadowed by an own property, a prototype
// swapped, a dictionary dropped and another made in its place, and a key of
// the same bits read from a different receiver.
function read(o, k) { return o[k]; }
function readMany(o, keys) {
    let s = '';
    for (const k of keys) s += String(read(o, k)) + ',';
    return s;
}

// An id-keyed map: past the threshold of index-like keys it is a dictionary.
const map = Object.create(null);
for (let i = 0; i < 40; i++) map[i * 3] = 'v' + i;
const ids = [0, 3, 6, 9, 30, 117];
for (let r = 0; r < 4; r++) console.log(readMany(map, ids));
delete map[6];
console.log(readMany(map, ids));
map[6] = 'back';
map[500] = 'new';
console.log(readMany(map, ids), read(map, 500));
for (let i = 0; i < 40; i++) delete map[i * 3];
console.log(readMany(map, ids), Object.keys(map).join());
Object.defineProperty(map, 3, { get() { return 'getter'; }, configurable: true });
console.log(readMany(map, ids));
Object.defineProperty(map, 3, { value: 'data again', configurable: true, writable: true });
console.log(readMany(map, ids));

// String keys of a dictionary (a delete made it one).
const rec = { alpha: 1, beta: 2, gamma: 3 };
delete rec.beta;
const names = ['alpha', 'beta', 'gamma', 'delta'];
for (let r = 0; r < 3; r++) console.log(readMany(rec, names));
rec.beta = 'b2';
rec.delta = 'd';
console.log(readMany(rec, names));
delete rec.alpha;
console.log(readMany(rec, names));

// One site, many dictionaries: each made, read, dropped.
let total = '';
for (let n = 0; n < 30; n++) {
    const m = Object.create(null);
    for (let i = 0; i < 40; i++) m[i] = n * 100 + i;
    delete m[n];
    total += read(m, 7) + ':' + read(m, n) + ' ';
}
console.log(total);

// Prototype methods read by name.
class Base { hello() { return 'base hello'; } get g() { return 'getter g'; } }
class Mid extends Base {}
class Leaf extends Mid {}
const leaf = new Leaf();
const keys = ['hello', 'g', 'toString', 'missing'];
for (let r = 0; r < 3; r++) console.log(keys.map((k) => typeof read(leaf, k)).join(), read(leaf, 'hello')());
Base.prototype.hello = function () { return 'replaced'; };
console.log(read(leaf, 'hello')());
Mid.prototype.hello = function () { return 'mid'; };
console.log(read(leaf, 'hello')());
leaf.hello = 'own now';
console.log(read(leaf, 'hello'), read(new Leaf(), 'hello')());
Object.setPrototypeOf(Mid.prototype, { hello() { return 'swapped'; } });
delete Mid.prototype.hello;
console.log(read(new Leaf(), 'hello')());

// Same key bits, receivers that differ: a plain object, an array, a
// string, a function, null.
const plain = { 1: 'p1', length: 'plain len' };
const arr = ['a0', 'a1'];
for (const o of [plain, arr, 'str', plain, arr, function f(a, b) {}, plain]) {
    console.log(String(read(o, 1)), String(read(o, 'length')));
}
try { read(null, 1); } catch (e) { console.log(e instanceof TypeError); }

// A key a map lacks: the chain's answer (null-prototype map, a map whose
// prototype holds the key as data, as a getter, or is a proxy).
const lack = Object.create(null);
for (let i = 0; i < 40; i++) lack['k' + i] = i;
delete lack.k0;
const withProto = { a: 1 };
for (let i = 0; i < 40; i++) withProto[i] = i;
delete withProto[0];
Object.setPrototypeOf(withProto, { fromProto: 'data', 77: 'num on proto' });
const getterProto = Object.create({ get fromProto() { return 'getter on proto'; } });
for (let i = 0; i < 40; i++) getterProto[i] = i;
delete getterProto[1];
const proxyProto = Object.create(new Proxy({}, { get: (t, k) => 'trap ' + String(k) }));
for (let i = 0; i < 40; i++) proxyProto[i] = i;
delete proxyProto[1];
const lackKeys = ['k0', 'k1', 'fromProto', 77, 'toString', 'nope', 1, 2];
for (const o of [lack, withProto, getterProto, proxyProto, lack, withProto]) {
    console.log(lackKeys.map((k) => { const v = read(o, k); return typeof v === 'function' ? 'fn' : String(v); }).join());
}
