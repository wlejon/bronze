// Maps keyed by keys built per access (runtime/dict_keyed.cpp: reads, writes
// and deletes of a dictionary-mode plain object, a Number key's digits probed
// without a string, a fresh string key probed by content). Keys that look
// like indices ("0", "01", "-0", "4294967295", 4294967295, 2 ** 32), two-byte
// keys, the empty string, very long keys, keys equal by content but other
// string objects, symbols, a delete during for-in, insert/delete churn
// (tombstones, compaction, rehash), a prototype holding the key (data,
// accessor, non-writable), a non-extensible map, non-writable and
// non-configurable entries, strict refusals, and templates with Number
// pieces (concat.n). Trained and run in one process (run.py's switch mode).
const TRAINING = Math.random() < 0.5;

function get(m, k) { return m[k]; }
function set(m, k, v) { m[k] = v; }
function del(m, k) { return delete m[k]; }
function sdel(m, k) { 'use strict'; return delete m[k]; }
function sset(m, k, v) { 'use strict'; m[k] = v; }
function key3(a, b, c) { return `${a}:${b}:${c}`; }
function key2(a, b) { return `${a}${b}`; }

function dict(proto) {
  const m = proto === undefined ? {} : Object.create(proto);
  m.junk = 1;
  delete m.junk;
  return m;
}

function show(m) {
  const ks = Object.keys(m);
  return ks.length + '[' + ks.slice(0, 12).map((k) => k + '=' + String(m[k])).join(',') + ']';
}

function train() {
  const m = dict();
  let s = 0;
  for (let i = 0; i < 3000; i++) {
    const k = key3('t', i % 50, i & 7);
    set(m, k, i);
    s += get(m, k);
    s += get(m, key2('x', i)) === undefined ? 1 : 0;
    if ((i & 3) === 0) del(m, k);
    set(m, 1000 + (i % 40), i);
    s += get(m, 1000 + (i % 40));
    if ((i & 7) === 0) del(m, 1000 + (i % 40));
  }
  return s + ' ' + Object.keys(m).length;
}

function real() {
  const out = [];
  // Index-looking keys, by string and by Number.
  const m = dict();
  const ks = ['0', '01', '-0', '4294967295', '4294967294', '1.5', '1e3', '', ' 1'];
  for (let i = 0; i < ks.length; i++) set(m, ks[i], 'v' + i);
  out.push(show(m));
  out.push(get(m, 0), get(m, -0), get(m, 1), get(m, 4294967295), get(m, 4294967294),
           get(m, 1.5), get(m, 1000), get(m, 1e3), get(m, 2 ** 32), get(m, NaN));
  set(m, -0, 'negzero');
  set(m, 4294967295, 'max');
  set(m, 2 ** 32, 'big');
  set(m, 1e21, 'huge');
  set(m, 0.1, 'frac');
  out.push(get(m, '0'), get(m, '4294967295'), get(m, '4294967296'), get(m, '1e+21'), get(m, '0.1'));
  out.push(del(m, '01'), del(m, 4294967295), del(m, 99), get(m, '01'), get(m, 4294967295));
  out.push(show(m));
  // Two-byte keys, the empty string, long keys, equal content other objects.
  const w = dict();
  const two = ['été', '中文', 'café', 'aĀ'];
  for (const k of two) set(w, k, k.length);
  set(w, '', 'empty');
  const long1 = 'k'.repeat(300) + 'end';
  const long2 = 'k'.repeat(150) + 'k'.repeat(150) + 'end';
  set(w, long1, 'long');
  out.push(get(w, long2), get(w, 'k'.repeat(303)), get(w, ''), get(w, 'caf' + 'é'),
           get(w, String.fromCharCode(0x4e2d, 0x6587)), get(w, 'aĀ'));
  const lat = 'abc' + 'def';
  const wide = ('abcĀ'.slice(0, 3) + 'def');
  set(w, lat, 1);
  set(w, wide, 2);
  out.push(get(w, 'abcdef'), Object.keys(w).filter((k) => k === 'abcdef').length);
  for (let i = 0; i < 200; i++) set(w, key2('p', i), i);
  let hit = 0;
  for (let i = 0; i < 200; i++) hit += get(w, 'p' + String(i)) === i ? 1 : 0;
  out.push(hit, del(w, long2), get(w, long1), del(w, ''), get(w, ''));
  // Symbols.
  const s1 = Symbol('k'), s2 = Symbol('k');
  set(w, s1, 'sym1');
  out.push(get(w, s1), get(w, s2), get(w, 'Symbol(k)'), del(w, s1), get(w, s1));
  // Delete during for-in, of keys already visited (bronze still visits a key
  // deleted before it is reached: its for-in walks a snapshot).
  const f = dict();
  for (let i = 0; i < 20; i++) set(f, key2('f', i), i);
  for (let i = 0; i < 20; i += 2) set(f, 3000 + i, i);
  const seen = [];
  for (const k in f) {
    seen.push(k);
    if (k === 'f3') { del(f, 'f1'); del(f, 'f2'); del(f, 3010); set(f, 'late', 1); }
  }
  out.push(seen.join(','), Object.keys(f).join(','));
  // Churn: tombstones, compaction and rehash.
  const c = dict();
  let live = 0;
  for (let r = 0; r < 20; r++) {
    for (let i = 0; i < 500; i++) set(c, key3('c', r, i), i);
    for (let i = 0; i < 500; i++) if (i % 5 !== 0) del(c, key3('c', r, i));
    for (let i = 0; i < 300; i++) set(c, 70000 + r * 300 + i, r);
    for (let i = 0; i < 300; i += 3) del(c, 70000 + r * 300 + i);
  }
  for (let r = 0; r < 20; r++) {
    for (let i = 0; i < 500; i += 5) live += get(c, key3('c', r, i)) === i ? 1 : 0;
    for (let i = 0; i < 300; i++) live += get(c, 70000 + r * 300 + i) === (i % 3 === 0 ? undefined : r) ? 1 : 0;
  }
  const ck = Object.keys(c);
  out.push(live, ck.length, ck[0], ck[ck.length - 1], ck.slice(0, 3).join('|'));
  // A prototype that holds the key.
  const proto = { inh: 'from-proto', 77777: 'proto-num' };
  Object.defineProperty(proto, 'ro', { value: 'ro', writable: false });
  Object.defineProperty(proto, 'acc', { get() { return 'got'; }, set(v) { out.push('setter ' + v); } });
  const p = dict(proto);
  out.push(get(p, 'in' + 'h'), get(p, 77777), get(p, 'r' + 'o'), get(p, 'ac' + 'c'));
  set(p, 'in' + 'h', 'own');
  set(p, 'r' + 'o', 'changed');
  set(p, 'ac' + 'c', 'value');
  set(p, 77777, 'own-num');
  out.push(get(p, 'inh'), proto.inh, get(p, 'ro'), Object.keys(p).join(','), get(p, 77777), proto[77777]);
  try { sset(p, 'ro', 1); out.push('no throw'); } catch (e) { out.push(e.constructor.name); }
  // Non-writable and non-configurable entries, a non-extensible map.
  const n = dict();
  set(n, 'a1', 1);
  Object.defineProperty(n, 'fixed', { value: 'f', writable: false, configurable: false, enumerable: true });
  set(n, 'fix' + 'ed', 'g');
  out.push(get(n, 'fixed'), del(n, 'fix' + 'ed'), get(n, 'fixed'));
  try { sdel(n, 'fix' + 'ed'); out.push('no throw'); } catch (e) { out.push(e.constructor.name); }
  try { sset(n, 'fix' + 'ed', 1); out.push('no throw'); } catch (e) { out.push(e.constructor.name); }
  Object.preventExtensions(n);
  set(n, 'new' + 'key', 1);
  set(n, 12345, 1);
  set(n, 'a' + '1', 2);
  out.push(get(n, 'newkey'), get(n, 12345), get(n, 'a1'), del(n, 'a1'), get(n, 'a1'));
  try { sset(n, 'other', 1); out.push('no throw'); } catch (e) { out.push(e.constructor.name); }
  Object.freeze(n);
  set(n, 'fixed', 'h');
  out.push(get(n, 'fixed'), Object.isFrozen(n));
  // A map that becomes a prototype, and a getter defined on a map entry.
  const q = dict();
  set(q, 'z' + 1, 'own');
  const child = Object.create(q);
  out.push(get(child, 'z1'), get(q, 'z1'));
  Object.defineProperty(q, 'g' + 1, { get() { return 'getter'; }, configurable: true });
  out.push(get(q, 'g1'), del(q, 'g' + 1), get(q, 'g1'));
  // Templates with Number pieces.
  out.push(key3(-0, 1e21, 0.1), key3(NaN, -Infinity, 2 ** 53), key3(1023, 1024, -1),
           key3('é', 5, 'x'.repeat(260)).length, key2(1.5e-7, 123456789012));
  return out.join(' ');
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(train());
} else {
  console.log(train());
  console.log(real());
}
