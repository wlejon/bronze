// A METHOD PARAMETER WHOSE CLASS IS A GUESS, SPENT ON A SHAPE GUARD.
//
// `Sink.take` below is reached two ways: once with a `new Pt(...)` the analysis
// watched being made, and once per element of an array, which is a value it can
// say nothing about. The parameter's TYPE is therefore `dynamic` and stays so —
// no unbox, no primitive field claim, no calling convention changes. What the
// callers that DID name a class agreed on is a separate fact
// (types/method_ident.h `paramShapes`), and lowering may spend it on one thing:
// the property-site family guard (src/lower/lower_infer.cpp `claimStaticSlot`,
// src/runtime/class_family.cpp), which asks the runtime whether the shape in
// hand is one of the layouts that BEGIN with `Pt`'s fields.
//
// So every line here is a receiver the guess is wrong about in a different way,
// and each one has to reach the answer 10.1.8.1 gives by NAME: a subclass, a
// plain literal with the three names, the same names in another order, an
// unrelated class that declares them backwards, a prototype hit, a redefined
// accessor, a dictionary-mode instance, a Proxy, a number, and null. A guard
// that matched any of them wrongly would read `Pt`'s slot 0 out of an object
// that keeps something else there.
//
// `Sink.take` is also the run shape chunk 28 admits a named read into — three
// constant-index element stores, then a read between each pair — so these are
// the answers with the shape question hoisted to the head of the span.
//
// This file is a Script. Nothing in it depends on strictness.

function show(a) {
  let s = '';
  for (let i = 0; i < a.length; i++) {
    if (i > 0) s += ',';
    s += String(a[i]);
  }
  return s;
}

class Pt {
  constructor(x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
  }
}

// A subclass: its fields BEGIN with the base's, so slot 0 really is `x` here
// too, and the family guard is meant to cover it.
class Pt4 extends Pt {
  constructor(x, y, z, w) {
    super(x, y, z);
    this.w = w;
  }
}

// The same three names, installed in the OPPOSITE order, by a class that
// extends nothing Pt extends. Slot 0 holds `z`; a guard that let this through
// would answer `z` for `x`.
class Other {
  constructor(z, y, x) {
    this.z = z;
    this.y = y;
    this.x = x;
  }
}

class Sink {
  constructor() {
    this.out = [0, 0, 0, 0, 0, 0];
  }
  take(p) {
    const o = this.out;
    o[0] = 1;
    o[1] = 2;
    o[2] = 3;
    o[3] = p.x;
    o[4] = p.y;
    o[5] = p.z;
    return show(o);
  }
}

const s = new Sink();

// The call that names a class. It is the only argument to `take` in this whole
// file whose type is proven, so `Pt` is what the guess is.
console.log('named|' + s.take(new Pt(10, 20, 30)));

// Everything below reaches `take` through an array element, which is a value
// with no type at all — so it contributes nothing to the guess and every one of
// these receivers arrives at a site that expects a `Pt`.
function protoOnly() {
  return Object.create({ x: 11, y: 22, z: 33 });
}

function getterOverSlot() {
  const v = new Pt(1, 2, 3);
  Object.defineProperty(v, 'y', { get: function () { return 55; }, configurable: true });
  return v;
}

function dictionaryMode() {
  const v = new Pt(1, 2, 3);
  v.tmp = 9;
  delete v.tmp;
  return v;
}

const trapLog = [];

function proxyReceiver() {
  return new Proxy({ x: 1, y: 2, z: 3 }, {
    get: function (t, k) {
      trapLog.push(String(k));
      return t[k] * 11;
    }
  });
}

const recv = [];
recv.push(new Pt(1, 2, 3));
recv.push(new Pt4(4, 5, 6, 7));
recv.push({ x: 7, y: 8, z: 9 });
recv.push({ z: 90, y: 80, x: 70 });
recv.push(new Other(300, 200, 100));
recv.push(protoOnly());
recv.push(getterOverSlot());
recv.push(dictionaryMode());
recv.push(proxyReceiver());
recv.push(5);

const labels = ['pt', 'sub', 'literal', 'reordered', 'other', 'proto',
                'getter', 'dict', 'proxy', 'number'];

for (let i = 0; i < recv.length; i++) {
  console.log(labels[i] + '|' + s.take(recv[i]));
}

console.log('traps|' + trapLog.join('.'));

// A receiver with no properties at all: 10.1.8.1 is never reached, because
// GetV on `null` throws before it (13.3.2.1 / 7.3.2). The three element stores
// in front of the read stand, and the three behind it were never made — a span
// emitted as two arms must not store on a path the program did not reach.
function nullReceiver() {
  const t = new Sink();
  const holes = [null];
  let name = 'no-throw';
  try {
    t.take(holes[0]);
  } catch (e) {
    name = e.name;
  }
  return name + '|' + show(t.out);
}

console.log('null|' + nullReceiver());

// The same, from a getter that throws in the MIDDLE of the span, on a receiver
// whose class the guess is right about.
function throwsMidSpan() {
  const t = new Sink();
  const v = new Pt(1, 2, 3);
  Object.defineProperty(v, 'z', {
    get: function () { throw new RangeError('mid'); },
    configurable: true
  });
  let name = 'no-throw';
  try {
    t.take(v);
  } catch (e) {
    name = e.name;
  }
  return name + '|' + show(t.out);
}

console.log('threw|' + throwsMidSpan());

// A WRITE through a guessed-class parameter, which needs the slot to be
// WRITABLE as well as present. `Pt`'s are, because a property created by
// assignment is; one redefined with `writable: false` is not, and 10.1.9.1
// answers false for it — which a class body, being strict code (15.7), turns
// into a TypeError. The runtime stamps a shape for the family guard only where
// it owns the slot as a writable data property, so the redefined instance is a
// shape the guard has to keep missing on.
class Bump {
  raise(p) {
    try {
      p.x = p.x + 1;
      return String(p.x);
    } catch (e) {
      return e.name + '|' + String(p.x);
    }
  }
}

const b = new Bump();
console.log('bump|' + b.raise(new Pt(1, 2, 3)));

const frozenish = [];
const locked = new Pt(1, 2, 3);
Object.defineProperty(locked, 'x', { value: 41, writable: false, configurable: true });
frozenish.push(locked);
console.log('locked|' + b.raise(frozenish[0]));
