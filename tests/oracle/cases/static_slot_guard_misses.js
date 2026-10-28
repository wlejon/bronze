// Static-slot sites whose layout claim stops describing the receiver.
//
// A site whose class layout proved its key's slot reads and writes that slot at
// a constant offset behind one guard (il_static_slot.cpp), and a stretch of such
// sites on one receiver shares one guard for all of them
// (il_lowering_static_run.cpp). Each case warms the sites until their cells are
// published or their shapes stamped, then hands them a receiver the claim is
// wrong about. Every line must read as the ordinary [[Get]] / [[Set]] would.

const N = 3000;

// Seven fields, so `mass`, `tag` and `link` live past the four inline slots,
// in the overflow block.
class Body {
  constructor(x, y) {
    this.x = x;
    this.y = y;
    this.vx = 1;
    this.vy = 2;
    this.mass = 3;
    this.tag = 'b';
    this.link = null;
  }
  // A run on `this`: three reads, three writes, arithmetic between.
  step() {
    this.x = this.x + this.vx;
    this.y = this.y + this.vy;
    this.mass = this.mass + 1;
    return this.x * 100 + this.y;
  }
  // A run whose writes store references (a barrier each), then a branch on a
  // value the run read, then the receiver again.
  relink(other, name) {
    this.link = other;
    this.tag = name;
    const m = this.mass;
    if (m > 10) return 'big:' + this.tag + ':' + (this.link === other);
    return 'small:' + this.tag + ':' + m;
  }
}

// A subclass with Body's fields first: the family guard admits it.
class Heavy extends Body {
  constructor(x, y) {
    super(x, y);
    this.extra = 9;
  }
}

// A subclass whose constructor moves `vx` past the others: the family stamp
// must not admit its shape, whose slot 2 is no longer `vx`.
class Shuffled extends Body {
  constructor(x, y) {
    super(x, y);
    delete this.vx;
    this.vx = 50;
  }
}

// Every call inference can see passes a Body or a subclass; the calls it
// cannot see (through `callers`) pass anything.
function energy(b) {
  return b.mass * (b.vx * b.vx + b.vy * b.vy);
}
const callers = [energy];

// A receiver whose class is exact by construction: its sites pin one shape in
// a cell (the identity form) rather than admitting a family.
const solo = new Body(0, 0);
function tickSolo() {
  solo.x = solo.x + solo.vx;
  solo.mass = solo.mass + solo.vy;
  return solo.x * 1000 + solo.mass;
}

function warmBody() {
  const b = new Body(0, 0);
  let s = 0;
  for (let i = 0; i < N; i++) {
    b.x = 0; b.y = 0; b.mass = 3;
    s = b.step() + energy(b);
  }
  return s;
}
console.log('warm', warmBody());

// 1. A subclass instance, and one whose layout only looks like Body's.
const h = new Heavy(1, 1);
console.log('heavy', h.step(), h.x, h.y, h.mass, h.extra, energy(h));
const sh = new Shuffled(1, 1);
console.log('shuffled', sh.step(), sh.x, sh.vx, sh.mass, energy(sh), Object.keys(sh).join(','));

// 2. A receiver of another shape entirely, reached where inference saw only
//    Bodies: the method borrowed onto a literal, and `energy` called through a
//    value it cannot see.
const lit = { vx: 5, vy: 6, x: 7, y: 8, mass: 1 };
console.log('literal', Body.prototype.step.call(lit), lit.x, lit.y, lit.mass, Object.keys(lit).join(','));
console.log('literal energy', callers[0]({ mass: 2, vy: 1, vx: 3 }));

// 3. A property deleted after warm-up: the read finds nothing, the write adds
//    it back at the end.
const d = new Body(0, 0);
for (let i = 0; i < N; i++) d.step();
delete d.vx;
console.log('deleted', d.step(), d.x, String(d.vx), energy(d));
d.vx = 4;
console.log('re-added', d.step(), d.x, energy(d), Object.keys(d).join(','));

// The identity form, warm, then the one object it pinned reshaped under it.
let t = 0;
for (let i = 0; i < N; i++) t = tickSolo();
console.log('solo', t, solo.x, solo.mass);
delete solo.vy;
console.log('solo deleted', String(tickSolo()), solo.x, String(solo.mass));
solo.mass = 1;
solo.vy = 2;
console.log('solo re-added', tickSolo(), Object.keys(solo).join(','));
Object.defineProperty(solo, 'vx', { get() { return -1; }, configurable: true });
console.log('solo accessor', tickSolo(), solo.x, solo.mass);

// The identity form as a run: one local receiver, reshaped partway through
// the loop that warmed its sites.
function soloRun(k, how) {
  const b = new Body(0, 0);
  let sets = 0;
  for (let i = 0; i < N; i++) {
    if (i === k) how(b, () => sets++);
    b.x = b.x + b.vx;
    b.mass = b.mass + b.vy;
  }
  return b.x + ':' + b.mass + ':' + sets;
}
console.log('soloRun never', soloRun(-1, null));
console.log('soloRun delete', soloRun(N - 10, (b) => { delete b.vy; }));
console.log('soloRun accessor', soloRun(N - 10, (b, count) => {
  Object.defineProperty(b, 'x', { get() { return 5; }, set(v) { count(); }, configurable: true });
}));
console.log('soloRun extra field', soloRun(N - 10, (b) => { b.more = 1; }));

// 4. A data property turned into an accessor after warm-up.
const a = new Body(0, 0);
for (let i = 0; i < N; i++) a.step();
const seen = [];
Object.defineProperty(a, 'x', {
  get() { return 1000; },
  set(v) { seen.push(v); },
  configurable: true,
});
console.log('accessor', a.step(), a.x, seen.join(','), energy(a));
Object.defineProperty(a, 'mass', { value: 7, writable: false });
let threw = '';
try { a.step(); } catch (e) { threw = e.constructor.name; }
console.log('read-only', threw, a.mass, seen.join(','));

// 5. Frozen after warm-up: every write of the run must refuse, and the reads
//    before the first one must still have happened.
const f = new Body(2, 3);
for (let i = 0; i < N; i++) { f.x = 2; f.y = 3; f.step(); }
f.x = 2; f.y = 3; f.mass = 3;
Object.freeze(f);
threw = '';
try { f.step(); } catch (e) { threw = e.constructor.name; }
console.log('frozen', threw, f.x, f.y, f.mass);

// 6. Not an object at all: a number reads through Number.prototype, a null
//    receiver throws before any read.
console.log('number', String(callers[0](5)));
Number.prototype.mass = 2;
Number.prototype.vx = 1;
Number.prototype.vy = 1;
console.log('number proto', callers[0](5));
delete Number.prototype.mass;
delete Number.prototype.vx;
delete Number.prototype.vy;
threw = '';
try { callers[0](null); } catch (e) { threw = e.constructor.name; }
console.log('null', threw);

// 7. The reference-storing run, warm, then on a subclass and a borrowed
//    literal, with the branch on the value it read taken both ways.
const r = new Body(0, 0);
const other = new Body(9, 9);
let last = '';
for (let i = 0; i < N; i++) last = r.relink(other, 't' + (i % 3));
console.log('relink', last, r.tag, r.link === other);
r.mass = 11;
console.log('relink big', r.relink(h, 'hh'), r.link === h);
console.log('relink heavy', h.relink(r, 'h2'), h.tag);
const lit2 = { mass: 12 };
console.log('relink literal', Body.prototype.relink.call(lit2, r, 'L'), Object.keys(lit2).join(','));
const pieces = [];
for (let i = 0; i < 50; i++) {
  const o = new Body(i, i);
  o.relink(pieces.length ? pieces[pieces.length - 1] : null, 'p' + i);
  pieces.push(o);
}
let depth = 0;
for (let p = pieces[49]; p !== null; p = p.link) depth++;
console.log('chain', depth, pieces[49].tag, pieces[49].link.tag, pieces[0].link);
