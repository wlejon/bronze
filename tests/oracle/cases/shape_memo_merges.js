// The shape memo (il2mir/il_shape_memo.cpp) and the class-family guards on
// both sides of a block with more than one predecessor. A memo that reached
// such a block (it starts empty there today; carrying it in as block
// parameters was measured and not kept, ledger chunk 18) would have to take,
// on each edge, what that predecessor knew at its end, or read the receiver
// afresh where the predecessor forgot it; and a family answer may be reused
// only while the keyed shape it was computed from stands.
//
// Every method below reads `this` (or a parameter) before a branch, does
// something in one arm that may or may not move the receiver's shape, and
// reads it again after the arms meet. They are warmed with arms that change
// nothing, so every cell is published and the reads after the merge have a
// memo to reuse; then each arm is made to reshape the receiver: through a
// getter on another object (a static site's miss), a delete, an added
// property, a prototype change, a freeze, an accessor in place of a field.
// Every line must read as the ordinary [[Get]] / [[Set]] would.

const N = 2000;

class Src {
  constructor() {
    this.g = 1;
  }
}
const src = new Src();
let act = () => {};

class Base {
  constructor() {
    this.a = 1;
    this.b = 2;
  }
  // this.a, then an arm that reads src.g (a static site whose miss may run
  // anything), the other arm only arithmetic; this.a and this.b after.
  ifElse(c) {
    const p = this.a;
    let q;
    if (c) {
      q = src.g;
    } else {
      q = p * 2;
    }
    return p + ':' + q + ':' + this.a + ':' + this.b;
  }
  // The arm deletes or adds on `this` itself when told to.
  armEdit(c, how) {
    const p = this.a;
    if (c) {
      if (how === 1) delete this.a;
      else if (how === 2) this.extra = 'e';
      else if (how === 3) Object.defineProperty(this, 'b', { get() { return 'gb'; } });
    }
    return p + ':' + this.a + ':' + this.b + ':' + this.extra;
  }
  // A loop: the header is entered from before the loop and from its back
  // edge, the body reads this.a and src.g.
  loop(n) {
    let s = '';
    for (let i = 0; i < n; i++) {
      s += this.a + ',' + src.g + ',' + this.b + ';';
    }
    return s;
  }
  // Writes on both sides of a merge: a write the object refuses throws.
  writes(c) {
    this.a = 5;
    if (c) {
      src.g;
    }
    this.a = 6;
    this.b = 7;
    return this.a + ':' + this.b;
  }
  // A switch: several predecessors into one block.
  sw(k) {
    const p = this.a;
    let q = 0;
    switch (k) {
      case 0: q = 1; break;
      case 1: q = src.g; break;
      case 2: q = this.b; break;
      default: q = -1;
    }
    return p + ':' + q + ':' + this.a + ':' + this.b;
  }
  // A throw in the middle of a block with a memo: the handler is entered
  // from inside the try, after src.g ran.
  tried(c) {
    const p = this.a;
    let q = 0;
    try {
      q = src.g;
      if (c) throw new Error('t');
      q = q + this.b;
    } catch (e) {
      q = 'caught:' + this.a + ':' + this.b;
    }
    return p + ':' + q + ':' + this.a;
  }
  // A ternary on both sides of which the same block is reached.
  same(c) {
    const p = this.a;
    const q = c ? src.g : src.g;
    return p + ':' + q + ':' + this.b;
  }
}

// A subclass: the family answer for Base's slots holds for it too, until it
// is reshaped.
class Sub extends Base {
  constructor() {
    super();
    this.c = 3;
  }
}

// A parameter, not `this`, as the receiver the merge carries.
function onParam(o, c) {
  const p = o.a;
  if (c) act();
  else src.g;
  return p + ':' + o.a + ':' + o.b;
}

// A generator method: the memo must not outlive a yield.
class Gen {
  constructor() {
    this.a = 1;
    this.b = 2;
  }
  *walk() {
    const p = this.a;
    yield p;
    if (this.b > 0) yield this.a + ':' + this.b;
    else yield 'neg';
    yield this.a;
  }
}

let warm = 0;
for (let i = 0; i < N; i++) {
  const o = i % 3 === 0 ? new Sub() : new Base();
  warm += o.ifElse(i & 1).length + o.armEdit(false, 0).length + o.loop(2).length;
  warm += o.writes(i & 1).length + o.sw(i % 4).length + o.tried(false).length + o.same(i & 1).length;
  warm += onParam(o, i & 1).length;
  const g = new Gen();
  for (const v of g.walk()) warm += String(v).length;
}
console.log('warm', warm);

Object.defineProperty(src, 'g', { get() { act(); return 7; } });
let b;
function run(Cls, method, what, ...args) {
  b = new Cls();
  act = what;
  let out;
  try {
    out = b[method](...args);
  } catch (e) {
    out = e.constructor.name + ':' + b.a + ':' + b.b;
  }
  act = () => {};
  return out;
}

for (const Cls of [Base, Sub]) {
  const n = Cls.name;
  // The arm that reads src.g deletes this.a / turns b into an accessor /
  // gives this a new prototype / adds a property; the other arm leaves it.
  console.log(n, 'ifElse delete', run(Cls, 'ifElse', () => { delete b.a; }, true));
  console.log(n, 'ifElse plain', run(Cls, 'ifElse', () => { delete b.a; }, false));
  console.log(n, 'ifElse accessor', run(Cls, 'ifElse', () => Object.defineProperty(b, 'b', { get() { return 'acc'; } }), true));
  console.log(n, 'ifElse proto', run(Cls, 'ifElse', () => { delete b.b; Object.setPrototypeOf(b, { b: 'pb' }); }, true));
  console.log(n, 'ifElse add', run(Cls, 'ifElse', () => { b.z = 1; delete b.a; b.a = 'readd'; }, true));
  console.log(n, 'ifElse widen', run(Cls, 'ifElse', () => { b.a = 'wide'; }, true));

  // The arm edits `this` directly.
  console.log(n, 'armEdit delete', run(Cls, 'armEdit', () => {}, true, 1));
  console.log(n, 'armEdit add', run(Cls, 'armEdit', () => {}, true, 2));
  console.log(n, 'armEdit getter', run(Cls, 'armEdit', () => {}, true, 3));
  console.log(n, 'armEdit none', run(Cls, 'armEdit', () => {}, false, 1));

  // Reshaped on the second iteration of a loop: the back edge carries what
  // the body's end knew.
  let k = 0;
  console.log(n, 'loop', run(Cls, 'loop', () => {
    if (++k === 2) { delete b.a; Object.defineProperty(b, 'b', { value: 'lb' }); }
  }, 4));
  k = 0;
  console.log(n, 'loop proto', run(Cls, 'loop', () => {
    if (++k === 3) Object.setPrototypeOf(b, { a: 'pa', b: 'pb2' });
    if (k === 3) { delete b.a; delete b.b; }
  }, 4));

  // Frozen or made read-only in the arm between two writes: the second
  // write throws (class code is strict).
  console.log(n, 'writes freeze', run(Cls, 'writes', () => Object.freeze(b), true));
  console.log(n, 'writes readonly', run(Cls, 'writes', () => Object.defineProperty(b, 'b', { writable: false }), true));
  let seen = '';
  console.log(n, 'writes setter', run(Cls, 'writes', () => Object.defineProperty(b, 'a', {
    get() { return 'sa'; },
    set(v) { seen += v; },
  }), true), seen);
  console.log(n, 'writes plain', run(Cls, 'writes', () => Object.freeze(b), false));

  for (let s = 0; s < 4; s++) {
    console.log(n, 'sw', s, run(Cls, 'sw', () => { delete b.a; b.b = 'swb'; }, s));
  }

  console.log(n, 'tried throw', run(Cls, 'tried', () => { delete b.a; }, true));
  console.log(n, 'tried', run(Cls, 'tried', () => { delete b.b; }, false));
  console.log(n, 'same', run(Cls, 'same', () => { delete b.b; }, true));
  console.log(n, 'same2', run(Cls, 'same', () => { Object.defineProperty(b, 'b', { get() { return 'sb'; } }); }, false));
}

// The parameter receiver: reshaped by act() in one arm, by src.g's getter
// in the other.
{
  const o = new Sub();
  act = () => { delete o.a; };
  console.log('param act', onParam(o, true));
  const o2 = new Base();
  act = () => { Object.defineProperty(o2, 'b', { get() { return 'pg'; } }); };
  console.log('param getter', onParam(o2, false));
  act = () => {};
  // Not an object of either class at all.
  console.log('param plain', onParam({ a: 'x', b: 'y' }, false));
  console.log('param proxy', onParam(new Proxy({ a: 'pa', b: 'pb' }, {}), true));
}

// Reshaped while the generator is suspended.
{
  const g = new Gen();
  const it = g.walk();
  const out = [it.next().value];
  delete g.a;
  Object.defineProperty(g, 'b', { get() { return 5; } });
  out.push(it.next().value);
  g.a = 'late';
  out.push(it.next().value);
  console.log('gen', out.join('|'));
  const g2 = new Gen();
  const it2 = g2.walk();
  it2.next();
  g2.b = -1;
  console.log('gen neg', it2.next().value, it2.next().value);
}

// A method borrowed onto objects of other shapes, after warm-up.
console.log('borrow', Base.prototype.ifElse.call({ a: 'oa', b: 'ob' }, true));
console.log('borrow arr', Base.prototype.sw.call(Object.assign([], { a: 'ra', b: 'rb' }), 2));
console.log('borrow frozen', Base.prototype.armEdit.call(Object.freeze({ a: 'fa', b: 'fb' }), false, 0));
