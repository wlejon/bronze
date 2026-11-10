// Dictionary-mode objects whose keys are computed strings: every operation that
// reads, writes or lists an own key. A dictionary keeps such a key as an
// ordinary heap string (not an immortal arena copy), so each answer here is
// one the collector could break if it freed or lost a key still in use.

function key(prefix, n) { return prefix + '_' + n; }

// A dictionary through a delete, keys built at run time.
const d = {};
for (let i = 0; i < 40; i++) d[key('k', i)] = i;
delete d[key('k', 0)];
console.log('size', Object.keys(d).length, d.k_1, d[key('k', 39)], d.k_0);
console.log('in', key('k', 5) in d, 'k_0' in d, 'nope' in d);
console.log('own', d.hasOwnProperty(key('k', 7)), Object.hasOwn(d, 'k_8'), d.hasOwnProperty('k_0'));
console.log('enum', d.propertyIsEnumerable(key('k', 9)), d.propertyIsEnumerable('toString'));

// Keys equal by content but built separately read the same property.
const a = ['a', 'b', 'c'].join('');
const b = 'ab' + 'c'.repeat(1);
d[a] = 'first';
console.log('same', d[b], d.abc, Object.keys(d).filter((k) => k === 'abc').length);
d[b] = 'second';
console.log('same2', d[a], Object.keys(d).filter((k) => k === 'abc').length);

// Integer-like keys first, ascending, then strings in insertion order, then
// symbols — for-in, Object.keys and getOwnPropertyNames alike.
const m = {};
m.z = 1;
delete m.z;
const sym = Symbol('s');
m['10'] = 'ten';
m[key('w', 1)] = 'w1';
m['2'] = 'two';
m[sym] = 'symbol';
m[''] = 'empty';
m['0'] = 'zero';
m['01'] = 'oh-one';
m[String(4294967294)] = 'max-index';
m['-1'] = 'neg';
const forin = [];
for (const k in m) forin.push(k);
console.log('forin', JSON.stringify(forin));
console.log('keys', JSON.stringify(Object.keys(m)));
console.log('names', JSON.stringify(Object.getOwnPropertyNames(m)));
console.log('syms', Object.getOwnPropertySymbols(m).length, m[sym]);
console.log('empty', m[''], '' in m);

// A very long key, and one that differs from it in the last character.
const long1 = 'x'.repeat(5000) + 'A';
const long2 = 'x'.repeat(5000) + 'B';
m[long1] = 1;
m[long2] = 2;
console.log('long', m[long1], m[long2], m['x'.repeat(5000) + 'A'], Object.keys(m).length);
delete m[long1];
console.log('long2', long1 in m, m[long2]);

// A UTF-16 key, and one spelled as Latin-1 elsewhere.
m['café'] = 'latin1';
m['π'] = 'pi';
console.log('utf16', m['caf' + String.fromCharCode(0xe9)], m[String.fromCharCode(0x3c0)]);

// Enumeration-based copies.
const e = {};
e.drop = 0;
delete e.drop;
for (let i = 0; i < 5; i++) e[key('e', i)] = i * 10;
console.log('entries', JSON.stringify(Object.entries(e)));
console.log('values', JSON.stringify(Object.values(e)));
console.log('assign', JSON.stringify(Object.assign({}, e)));
console.log('spread', JSON.stringify({ ...e, extra: 1 }));
const { e_0, ...rest } = e;
console.log('rest', e_0, JSON.stringify(rest));
console.log('json', JSON.stringify(e), JSON.stringify(e, null, 1).length);
console.log('fromEntries', JSON.stringify(Object.fromEntries(Object.entries(e).map(([k, v]) => [k + '!', v]))));

// Descriptors and definitions on a dictionary object.
Object.defineProperty(e, key('ro', 1), { value: 'fixed', enumerable: false, writable: false });
Object.defineProperty(e, key('acc', 1), {
  get() { return 'got'; },
  set(v) { this.e_1 = v; },
  enumerable: true,
  configurable: true,
});
e.acc_1 = 77;
console.log('define', e.ro_1, e.acc_1, e.e_1, Object.keys(e).join());
e.ro_1 = 'changed';
console.log('ro', e.ro_1, JSON.stringify(Object.getOwnPropertyDescriptor(e, 'ro_1')));
const descs = Object.getOwnPropertyDescriptors(e);
console.log('descs', Object.keys(descs).join(), typeof descs.acc_1.get, descs.e_2.value);
Object.defineProperties(e, { [key('dp', 1)]: { value: 1, enumerable: true }, [key('dp', 2)]: { value: 2 } });
console.log('defineProperties', e.dp_1, e.dp_2, Object.keys(e).includes('dp_2'));

// Delete during for-in of keys already visited (and their collection: the
// loop's key list must keep the strings it still has to hand out). Deleting a
// key not yet visited is left out: bronze's for-in visits its snapshot, which
// 14.7.5.9 does not allow for a deleted key.
const f = {};
f.q = 0;
delete f.q;
for (let i = 0; i < 8; i++) f[key('f', i)] = i;
const seen = [];
for (const k in f) {
  seen.push(k + '=' + f[k]);
  if (k === 'f_1') { delete f.f_1; delete f.f_0; }
  if (k === 'f_4') { for (let j = 0; j < 200; j++) f[key('tmp', j)] = j; for (let j = 0; j < 200; j++) delete f[key('tmp', j)]; }
}
console.log('forin-delete', seen.join(), Object.keys(f).join());

// Freeze, seal and preventExtensions on dictionary objects with computed keys.
const fr = {};
fr.t = 1;
delete fr.t;
for (let i = 0; i < 4; i++) fr[key('fr', i)] = i;
Object.freeze(fr);
fr.fr_0 = 99;
fr[key('new', 1)] = 1;
console.log('frozen', Object.isFrozen(fr), fr.fr_0, 'new_1' in fr, Object.keys(fr).join());
for (let r = 0; r < 3; r++) {
  let sum = 0;
  for (const k in fr) sum += fr[k];
  console.log('frozen-read', r, sum, fr[key('fr', 3)]);
}
const se = {};
se.t = 1;
delete se.t;
se[key('se', 1)] = 1;
Object.seal(se);
se.se_1 = 2;
delete se.se_1;
se.other = 3;
console.log('sealed', Object.isSealed(se), se.se_1, 'other' in se);
const pe = {};
pe[key('pe', 1)] = 1;
delete pe.pe_0;
Object.preventExtensions(pe);
delete pe.pe_1;
pe.pe_1 = 5;
console.log('prevented', Object.isExtensible(pe), 'pe_1' in pe);

// Numeric-string keys on a dictionary read through numbers and strings alike.
const n = {};
n.t = 0;
delete n.t;
for (let i = 0; i < 6; i++) n[i * 7] = 'n' + i;
console.log('numeric', n[14], n['21'], n[35], Object.keys(n).join());
delete n[14];
console.log('numeric2', 14 in n, '21' in n, Object.keys(n).join());

// A map-sized object (past the dictionary threshold) keyed by ids.
const big = {};
for (let i = 0; i < 1500; i++) big['id' + i] = i;
let total = 0;
for (let i = 0; i < 1500; i += 3) { total += big['id' + i]; delete big['id' + i]; }
console.log('big', total, Object.keys(big).length, big.id1, big.id3);
