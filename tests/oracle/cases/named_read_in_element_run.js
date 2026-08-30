// NAMED PROPERTY READS STANDING INSIDE A RUN OF ELEMENT ACCESSES.
//
// A stretch of constant-index accesses off one receiver is proven once and
// spent per member (src/codegen-llvm/llvm_recv_proof.h): one length test, one
// base pointer, and each access a GEP and a load. What ENDS such a run is an
// instruction that can move the heap, and an `o.name` read between two of the
// accesses is one — its arms reach a getter, a Proxy trap and the property
// helper, any of which can reallocate or truncate the very array the proof
// describes. `Matrix4.compose` is the shape that puts the two together: twelve
// constant-index stores into `this.elements`, then `te[12] = position.x` three
// times.
//
// This case pins the ANSWERS around that interleaving, so that a change to
// where a run may span is checked against behaviour rather than against a
// plan. Each case below sends the named read down one of its arms and pins what
// the element accesses after it must then do:
//
//   THE GETTER TRUNCATES the very array the accesses are over, so a stale
//   length would let the stores after it land in an element block the array no
//   longer owns.
//   THE GETTER GROWS it past its capacity, so a stale base pointer would
//   address memory the array has moved out of.
//   THE FIELD IS REDEFINED between two reads of it, so the second read must
//   call a getter where the first read a slot.
//   THE RECEIVER IS A PROXY, whose trap must run once per read, in order.
//   THE READ THROWS, so the stores in front of it stand and the ones behind it
//   were never made.
//   THE SHAPE CHANGES between two reads of one object, which says nothing
//   about the array the accesses are over and must not disturb them.
//
// This file is a Script. Nothing in it depends on strictness.

function seq(n, base) {
  const a = [];
  for (let i = 0; i < n; i++) a.push(base + i);
  return a;
}

function show(a) {
  let s = '';
  for (let i = 0; i < a.length; i++) {
    if (i > 0) s += ',';
    s += String(a[i]);
  }
  return s;
}

// 1. The plain shape: element accesses and named reads alternating, nothing
//    unusual about any of them.
function copyWithFields(dst, src, o) {
  dst[0] = src[0];
  dst[1] = o.a;
  dst[2] = src[1];
  dst[3] = o.b;
  dst[4] = src[2];
  return dst;
}

console.log('plain|' + show(copyWithFields(seq(5, 0), [10, 20, 30], { a: 1, b: 2 })));

// 2. A getter that TRUNCATES the destination. The run's one length test cleared
//    index 3 on a six-element array; after the getter there are two.
function shrinkingGetter() {
  const dst = seq(6, 0);
  const o = {
    get a() {
      dst.length = 2;
      return 99;
    }
  };
  dst[0] = 100;
  dst[1] = o.a;
  dst[2] = 300;
  dst[3] = 400;
  return show(dst);
}

console.log('shrink|' + shrinkingGetter());

// 3. A getter that GROWS it far past its capacity, so the element block moves.
function growingGetter() {
  const dst = seq(4, 0);
  const o = {
    get a() {
      for (let i = 0; i < 40; i++) dst.push(i);
      return 7;
    }
  };
  dst[0] = 100;
  dst[1] = o.a;
  dst[2] = 300;
  return dst.length + '|' + dst[0] + ',' + dst[1] + ',' + dst[2] + ',' + dst[3] + ',' + dst[43];
}

console.log('grow|' + growingGetter());

// 4. The field redefined between two reads of it.
function redefinedMidRun() {
  const dst = seq(4, 0);
  const o = { a: 1, b: 2 };
  dst[0] = o.a;
  Object.defineProperty(o, 'b', { get: function () { return 55; }, configurable: true });
  dst[1] = o.b;
  dst[2] = o.a;
  return show(dst);
}

console.log('redefined|' + redefinedMidRun());

// 5. A Proxy receiver: the trap runs, once per read, in source order.
function proxyRead() {
  const dst = seq(4, 0);
  const log = [];
  const o = new Proxy({ a: 1, b: 2 }, {
    get: function (t, k) {
      log.push(String(k));
      return t[k] * 10;
    }
  });
  dst[0] = 9;
  dst[1] = o.a;
  dst[2] = 8;
  dst[3] = o.b;
  return show(dst) + '|' + log.join('.');
}

console.log('proxy|' + proxyRead());

// 6. The read throws. The store in front of it stands; the ones behind it were
//    never reached.
function throwsMidRun() {
  const dst = seq(4, 0);
  const o = {
    get a() {
      throw new RangeError('mid');
    }
  };
  let caught = '';
  try {
    dst[0] = 11;
    dst[1] = o.a;
    dst[2] = 33;
  } catch (e) {
    caught = e.name;
  }
  return caught + '|' + show(dst);
}

console.log('threw|' + throwsMidRun());

// 7. The read's receiver gains a property between two reads. The run is over a
//    DIFFERENT object, so this is the case the carry must survive.
function shapeChangesMidRun() {
  const dst = seq(4, 0);
  const o = { a: 1 };
  dst[0] = o.a;
  o.b = 2;
  dst[1] = o.a;
  dst[2] = o.b;
  return show(dst);
}

console.log('grew-shape|' + shapeChangesMidRun());

// 8. The named read is off THE ARRAY ITSELF — a named property of an Array
//    lives in the side object the slot arms read — and a getter later in the
//    same stretch truncates that array. So one run holds a named read on its
//    own receiver and a read that must kill the proof, in that order.
function readOffTheArray() {
  const a = seq(4, 0);
  a.tag = 5;
  const cut = {
    get k() {
      a.length = 1;
      return 7;
    }
  };
  a[0] = 100;
  a[1] = a.tag;
  a[2] = cut.k;
  a[3] = 400;
  return show(a) + '|' + a.tag;
}

console.log('self|' + readOffTheArray());

// 9. A named read on a primitive, which reaches no slot arm at all.
function primitiveRead() {
  const dst = seq(3, 0);
  const s = 'xyz';
  dst[0] = 7;
  dst[1] = s.length;
  dst[2] = 9;
  return show(dst);
}

console.log('primitive|' + primitiveRead());

// 10. `Matrix4.compose`'s own shape, twice: once with ordinary fields and once
//     with a getter that truncates the destination between two of the stores.
function composeShape(te, p) {
  te[0] = 1; te[1] = 2; te[2] = 3; te[3] = 0;
  te[4] = 4; te[5] = 5; te[6] = 6; te[7] = 0;
  te[8] = 7; te[9] = 8; te[10] = 9; te[11] = 0;
  te[12] = p.x;
  te[13] = p.y;
  te[14] = p.z;
  te[15] = 1;
  return te;
}

console.log('compose|' + show(composeShape(seq(16, 0), { x: 10, y: 20, z: 30 })));

function composeTruncating() {
  const te = seq(16, 0);
  const p = {
    x: 10,
    get y() {
      te.length = 5;
      return 20;
    },
    z: 30
  };
  composeShape(te, p);
  return te.length + '|' + te[4] + ',' + te[5] + ',' + te[12] + ',' + te[13] + ',' +
    te[14] + ',' + te[15];
}

console.log('compose-cut|' + composeTruncating());

// 11. The typed-array shape: own fields of a constructed object feeding a run
//     of typed element stores, which is `Color.toArray`.
function Vec(x, y, z) {
  this.x = x;
  this.y = y;
  this.z = z;
}

function toF32(out, o, off) {
  out[off] = o.x;
  out[off + 1] = o.y;
  out[off + 2] = o.z;
  return out;
}

function typedRun() {
  const f = new Float32Array(6);
  toF32(f, new Vec(0.5, 0.25, 0.125), 0);
  toF32(f, new Vec(1.5, 2.25, 4.125), 3);
  let s = '';
  for (let i = 0; i < f.length; i++) {
    if (i > 0) s += ',';
    s += String(f[i]);
  }
  return s;
}

console.log('typed|' + typedRun());
