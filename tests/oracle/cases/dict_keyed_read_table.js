// Computed reads of dictionary-mode objects answered from the thread's
// computed-read table (runtime/elem_ic.h elemCacheFillDict, the module's
// computed-read stub): `map[k]` of many dictionaries at one site, keyed by
// uid Numbers and by strings (constant, built by concatenation, numeric-like:
// "1", "01", "-0"), with the entry's answer changed under it by a delete, a
// re-add, a redefinition to an accessor, a freeze, a prototype that holds
// the key, `__proto__`, symbols, a Proxy receiver, and an object that turns
// into a dictionary after the site has been trained on shaped ones. Trained
// and run in one process (run.py's switch mode runs the real arm on a
// harvest of the training arm).
const TRAINING = Math.random() < 0.5;

function read(m, k) { return m[k]; }
function readTwice(m, k) { const a = m[k]; return a === undefined ? 'u' : a + '/' + m[k]; }

function dictOf(i) {
  const m = Object.create(null);
  m.junk = 1;
  delete m.junk;               // dictionary mode
  m[i] = 'n' + i;
  m['s' + i] = 'v' + i;
  m.ev = i * 2;
  return m;
}

const maps = [];
for (let i = 0; i < 300; i++) maps.push(dictOf(i));

function train() {
  let s = 0;
  for (let r = 0; r < 60; r++) {
    for (let i = 0; i < maps.length; i++) {
      const m = maps[i];
      s += read(m, i).length + read(m, 's' + i).length + read(m, 'ev');
      s += readTwice(m, 'ev').length;
    }
  }
  return s;
}

function real() {
  const out = [];
  const m = maps[7];
  out.push(read(m, 7), read(m, 's7'), read(m, 'ev'), read(m, 8), read(m, 's8'));
  // Numeric-like keys: "1" and 1 name one property; "01" and "-0" do not.
  const n = dictOf(1);
  out.push(read(n, '1'), read(n, 1), read(n, '01'), read(n, '-0'), read(n, -0), read(n, 1.5));
  n['-0'] = 'mz';
  n['01'] = 'z1';
  out.push(read(n, '-0'), read(n, -0), read(n, '01'), read(n, 0));
  // The entry's answer changes: delete, re-add, accessor, freeze.
  out.push(read(m, 'ev'));
  delete m.ev;
  out.push(read(m, 'ev'));
  m.ev = 'back';
  out.push(read(m, 'ev'));
  Object.defineProperty(m, 'ev', { get() { return 'getter'; }, configurable: true });
  out.push(read(m, 'ev'), readTwice(m, 'ev'));
  Object.defineProperty(m, 'ev', { value: 'data again', writable: true, configurable: true });
  out.push(read(m, 'ev'));
  Object.freeze(m);
  out.push(read(m, 'ev'), read(m, 7));
  // A prototype that holds the key, with a getter, and one with data.
  const proto = { get pk() { return 'from proto getter'; }, pd: 'proto data' };
  const withProto = Object.create(proto);
  withProto.x = 1; delete withProto.x;
  withProto.own = 'own';
  out.push(read(withProto, 'pk'), read(withProto, 'pd'), read(withProto, 'own'));
  withProto.pd = 'shadow';
  out.push(read(withProto, 'pd'));
  delete withProto.pd;
  out.push(read(withProto, 'pd'));
  // __proto__ on a null-prototype dictionary is an ordinary key.
  const np = dictOf(2);
  np['__proto__'] = 'just data';
  out.push(read(np, '__proto__'), Object.getPrototypeOf(np) === null);
  const op = {}; op.a = 1; delete op.a;
  out.push(typeof read(op, '__proto__'));
  // Symbols.
  const sym = Symbol('k');
  const sd = dictOf(3);
  sd[sym] = 'sym';
  out.push(read(sd, sym), read(sd, Symbol('k')));
  // Keys built by concatenation, fresh strings every time.
  let cat = 0;
  for (let i = 0; i < 300; i++) cat += read(maps[i], 's' + String(i)).length;
  out.push(cat);
  // A Proxy receiver and a Map at the trained site.
  const px = new Proxy(maps[5], { get(t, k) { return 'trap:' + String(k); } });
  out.push(read(px, 5), read(px, 'ev'), read(new Map([['ev', 1]]), 'ev'));
  // A shaped object, then the same object turned into a dictionary.
  const shaped = { ev: 'shaped', 9: 'nine' };
  out.push(read(shaped, 'ev'), read(shaped, 9));
  delete shaped[9];
  out.push(read(shaped, 'ev'), read(shaped, 9));
  shaped.ev = 'changed';
  out.push(read(shaped, 'ev'));
  // Many reads after a mutation of each map, so stale entries are asked.
  let tot = 0;
  for (let r = 0; r < 3; r++) {
    for (let i = 10; i < 300; i++) {
      const q = maps[i];
      if (r === 1) { q[i] = 'm' + i; delete q['s' + i]; }
      const v = read(q, i), w = read(q, 's' + i);
      tot += v.length + (w === undefined ? 100 : w.length);
    }
  }
  out.push(tot);
  return out.join(' ');
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(train());
} else {
  console.log(train());
  console.log(real());
}
