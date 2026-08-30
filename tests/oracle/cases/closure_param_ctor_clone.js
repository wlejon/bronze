// THE CLOSURE PARAMETER PROOF ACROSS A CONSTRUCTOR'S COPIED BODY.
//
// A class constructor is not lowered from the tree inference read: its body is
// COPIED so the field initializers can be spliced in (src/lower/lower_class.cpp),
// and the copy dies with the class. The closure parameter proof
// (src/lower/lower_scope.cpp, `planClosureParamNumbers`) hands a nested
// `function f(x)` an f64 parameter slot when every enumerable call site passes a
// Number, and it records that permission AGAINST THE DECLARATION NODE — a node
// which, inside a constructor, belongs to the copy and outlives nothing.
//
// Two constructors of the same SHAPE are what make that lifetime observable:
// the allocator hands the second copy the addresses the first one has just
// freed, so a permission that outlived the first copy is answered for the
// second's nodes. A parameter every call site passes a string to then arrives
// in an f64 slot, and what comes out is NaN — no throw, no diagnostic, and only
// on the runs where the addresses happen to line up, which is what made this a
// nondeterministic compiler rather than merely a wrong one.
//
// So each pair below is deliberately identical but for its arguments, and the
// pairs are written both ways round: the proven constructor first, and the
// unprovable one first. `typeof` is in most of the answers because the slot is
// exactly what `typeof` reports on.

// 0. The tightest pair there is: two constructors whose bodies differ only in
//    the literals, so the second copy lands on the first's freed nodes. `m` and
//    `o` read back as NaN when the permission crosses over.
class PickNum {
  constructor() {
    function pick(v, k) { return v; }
    this.a = pick(1, 2);
    this.b = pick(3, 4);
  }
}

class PickStr {
  constructor() {
    function pick(v, k) { return v; }
    this.a = pick('m', 'n');
    this.b = pick('o', 'p');
  }
}

const pn = new PickNum();
const ps = new PickStr();
console.log(pn.a, pn.b, ps.a, ps.b);

// 1. proven first, then the same shape called with strings.
class NumFirst {
  constructor() {
    function take(v, k) { return `${typeof v}:${v}/${typeof k}:${k}`; }
    this.one = take(1, 2);
    this.two = take(3, 4);
  }
}

class StrSecond {
  constructor() {
    function take(v, k) { return `${typeof v}:${v}/${typeof k}:${k}`; }
    this.one = take('a', 'b');
    this.two = take('c', 'd');
  }
}

console.log(new NumFirst().one, new NumFirst().two);
console.log(new StrSecond().one, new StrSecond().two);

// 2. the other way round, so the mirror of the same collision is pinned too.
class StrFirst {
  constructor() {
    function pass(v) { return `${typeof v}[${v}]`; }
    this.one = pass('m');
    this.two = pass('n');
  }
}

class NumSecond {
  constructor() {
    function pass(v) { return `${typeof v}[${v}]`; }
    this.one = pass(5);
    this.two = pass(6);
  }
}

console.log(new StrFirst().one, new StrFirst().two, new NumSecond().one, new NumSecond().two);

// 3. a DERIVED constructor, whose copy also carries spliced field statements —
//    a different splice, and so a different set of nodes freed at the end of it.
class Base {
  constructor() { this.tag = 'base'; }
}

class DerivedNum extends Base {
  field = 1;
  constructor() {
    super();
    function amount(x) { return `${typeof x}=${x}`; }
    this.one = amount(7);
    this.two = amount(8);
  }
}

class DerivedStr extends Base {
  field = 1;
  constructor() {
    super();
    function amount(x) { return `${typeof x}=${x}`; }
    this.one = amount('q');
    this.two = amount('r');
  }
}

const dn = new DerivedNum();
const ds = new DerivedStr();
console.log(dn.tag, dn.field, dn.one, dn.two);
console.log(ds.tag, ds.field, ds.one, ds.two);

// 4. the proof must still REACH into a constructor: a nested declaration every
//    site of which passes a Number is entitled to its f64 slot there like
//    anywhere else, and must compute the same answer with it.
class Summer {
  constructor(n) {
    function add(a, b) { return a + b; }
    let t = 0;
    for (let i = 0; i < n; i++) t = add(t, i);
    this.total = t;
  }
}

console.log(new Summer(5).total, new Summer(1).total);

// 5. one string site inside a constructor refuses the position for every site,
//    including the numeric ones — the join is over all of them.
class Mixed {
  constructor() {
    function show(x) { return `${typeof x}<${x}>`; }
    this.one = show(9);
    this.two = show('z');
  }
}

const mx = new Mixed();
console.log(mx.one, mx.two);
