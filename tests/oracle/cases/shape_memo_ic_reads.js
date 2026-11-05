// The shape memo carried across an inline-cache read (il2mir MemoCarry), and
// across the joins of an `if (a && r.x ...) else if (...)` chain, which the
// truth-join threading (lower/truth_join_threading.h) turns into branches.
// A receiver is read at its static sites, then an ordinary cached read — of
// another object, or of the receiver itself — whose miss runs a getter, and
// the receiver is read again. The reads are warmed while every cached read
// hits (an absent key, a plain data slot), so the memo is carried across
// each; then the getter arrives and reshapes the receiver between its reads:
// a delete, an added property, an accessor in place of a field, a frozen
// object, a new prototype. Every line must read as the ordinary [[Get]] does.

class M {
  constructor(i) {
    this.a = i;
    this.b = i + 1;
    this.c = i + 2;
  }
}

let victim = null;
let how = 0;
function reshape() {
  if (victim === null) return;
  if (how === 1) delete victim.a;
  else if (how === 2) victim.z = 'z';
  else if (how === 3) Object.defineProperty(victim, 'b', { get() { return -1; }, configurable: true });
  else if (how === 4) Object.freeze(victim);
  else if (how === 5) Object.setPrototypeOf(victim, { c: 'proto' });
  else if (how === 6) delete victim.c;
}

// Another object's read between the receiver's: absent while warm.
const plainProto = {};
const getterProto = {};
Object.defineProperty(getterProto, 'flag', {
  get() {
    reshape();
    return undefined;
  },
  configurable: true,
});

function chain(m, o) {
  let s = '';
  if (m.a === -5 && o.flag) s += 'x';
  else if (!o.flag && m.b === -5) s += 'y';
  else if (o.flag === 'q' || m.c === -5) s += 'z';
  else if (m.a !== m.b && o.other === undefined) s += 'w';
  return s + ':' + m.a + ',' + m.b + ',' + m.c + ',' + m.z;
}

// The receiver's own absent read, whose prototype later gains a getter.
class R {
  constructor(i) {
    this.a = i;
    this.b = i * 2;
  }
  test() {
    let s = '';
    if (this.a > 0 && this.q) s += 'q';
    else if (this.b !== 0 && !this.q) s += 'n';
    return s + ':' + this.a + ',' + this.b + ',' + this.z;
  }
}

// A polymorphic read of a second object between the receiver's reads.
function poly(m, o) {
  const x = m.a;
  const k = o.k;
  return x + ':' + k + ':' + m.a + ',' + m.b + ',' + m.c;
}

const out = [];
for (let mode = 1; mode <= 6; mode++) {
  for (let i = 0; i < 300; i++) chain(new M(i), Object.create(plainProto));
  const m = new M(mode);
  victim = m;
  how = mode;
  out.push('chain ' + mode + ' ' + chain(m, Object.create(getterProto)));
  out.push('again ' + mode + ' ' + chain(m, Object.create(plainProto)));
  victim = null;
}

for (let i = 0; i < 300; i++) new R(i + 1).test();
const r = new R(3);
Object.defineProperty(R.prototype, 'q', {
  get() {
    delete this.a;
    this.z = 'zz';
    return 0;
  },
  configurable: true,
});
out.push('own ' + r.test());
out.push('own2 ' + new R(4).test());

const shapes = [{ k: 1 }, { j: 0, k: 2 }, { i: 0, j: 0, k: 3 }];
for (let i = 0; i < 300; i++) poly(new M(i), shapes[i % 3]);
const pm = new M(7);
const tricky = {};
Object.defineProperty(tricky, 'k', {
  get() {
    delete pm.a;
    pm.c = 'cc';
    return 'got';
  },
});
out.push('poly ' + poly(pm, tricky));
out.push('poly2 ' + poly(new M(8), shapes[1]));

console.log(out.join('\n'));
