// A SPAN OF INTERLEAVED ACCESSES EMITTED AS TWO ARMS
// (src/codegen-llvm/llvm_run_arms.h), and every way the one branch in front of
// it can go the other way.
//
// The shape: a stretch of consecutive instructions holding a run of
// constant-index READS off one Array, a run of constant-index WRITES into
// another, and the arithmetic between them, emitted under ONE test that is the
// AND of both runs' proofs. The fast arm is loads, stores and machine
// arithmetic with no safepoint in it; the slow arm is the per-access cache
// ladder for the whole span; one join phis everything either arm defined.
// Four things have to survive that:
//
//   PROGRAM ORDER. The two arms perform the same accesses in the same
//   sequence, so a receiver that is BOTH the source and the destination reads
//   back what the store before it wrote. `bleed` is that pin: it copies element
//   i into element i+1 fifteen times, so every element ends as element zero —
//   and would not if the fast arm had hoisted its reads above its stores.
//
//   EXACTLY ONCE. Nothing the span writes is written twice and nothing it wrote
//   before it left is unwritten. `counted` puts a counting accessor on every
//   index of the destination, which refuses the store proof and sends the whole
//   span down the slow arm; the count is the number of stores the slow arm
//   made.
//
//   EITHER PROOF ALONE. The test is a conjunction, so a good source with a bad
//   destination and a bad destination with a good source both take the slow
//   arm, and the same sites are shown taking both.
//
//   WHAT WAS LIVE ACROSS. A value made in front of the span and read behind it
//   comes off the join: the fast arm's register on one edge, and on the other a
//   reload out of the slot, because the slow arm's ladders can run a getter and
//   collect.
//
// This file is a Script and therefore SLOPPY, which is what makes the frozen
// destination a discard rather than a throw; the strict spelling of the same
// span is below it and throws at its first store.

function seq(base) {
  const a = [];
  for (let i = 0; i < 16; i++) a.push(base + i);
  return a;
}

function show(a, n) {
  let s = '';
  for (let i = 0; i < n; i++) {
    if (i > 0) s += ',';
    s += String(a[i]);
  }
  return s;
}

// The span this file is about: sixteen reads off `src` interleaved with
// sixteen constant-index writes into `dst`, and nothing else between them.
// `tag` is made before the span and read after it, so it is live across.
function copy16(dst, src, tag) {
  const kept = { t: tag };
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
  dst[4] = src[4];
  dst[5] = src[5];
  dst[6] = src[6];
  dst[7] = src[7];
  dst[8] = src[8];
  dst[9] = src[9];
  dst[10] = src[10];
  dst[11] = src[11];
  dst[12] = src[12];
  dst[13] = src[13];
  dst[14] = src[14];
  dst[15] = src[15];
  return kept.t;
}

// The same span through a STRICT reference: 13.15.2 PutValue step 6.d turns the
// refused write into a TypeError instead of discarding it.
function copy16Strict(dst, src) {
  'use strict';
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
  dst[4] = src[4];
  dst[5] = src[5];
  dst[6] = src[6];
  dst[7] = src[7];
  dst[8] = src[8];
  dst[9] = src[9];
  dst[10] = src[10];
  dst[11] = src[11];
  dst[12] = src[12];
  dst[13] = src[13];
  dst[14] = src[14];
  dst[15] = src[15];
}

// One receiver read and written in the same span, with every read AFTER the
// store that overwrote it. Only program order gives element zero to all of
// them.
function bleed(a) {
  a[1] = a[0];
  a[2] = a[1];
  a[3] = a[2];
  a[4] = a[3];
  a[5] = a[4];
  a[6] = a[5];
  a[7] = a[6];
  a[8] = a[7];
  a[9] = a[8];
  a[10] = a[9];
  a[11] = a[10];
  a[12] = a[11];
  a[13] = a[12];
  a[14] = a[13];
  a[15] = a[14];
  return a;
}

// A span with no read run at all: sixteen constant-index stores with machine
// arithmetic between them, which is what `Matrix4.multiplyMatrices` ends with.
function fill16(out, x) {
  out[0] = x * 1;
  out[1] = x * 2;
  out[2] = x * 3;
  out[3] = x * 4;
  out[4] = x * 5;
  out[5] = x * 6;
  out[6] = x * 7;
  out[7] = x * 8;
  out[8] = x * 9;
  out[9] = x * 10;
  out[10] = x * 11;
  out[11] = x * 12;
  out[12] = x * 13;
  out[13] = x * 14;
  out[14] = x * 15;
  out[15] = x * 16;
  return out;
}

// 1. Both proofs hold: two dense sixteen-element Arrays, twice, because the
//    second call is the one whose sites have already cached everything.
const d1 = seq(100);
console.log(copy16(d1, seq(0), 'one') + '|' + show(d1, 16));
const d2 = seq(100);
console.log(copy16(d2, seq(0), 'two') + '|' + show(d2, 16));

// 2. The source IS the destination, and the copy is the identity.
const same = seq(0);
console.log(copy16(same, same, 'same') + '|' + show(same, 16));

// 3. And the same receiver where the order is observable.
console.log(show(bleed(seq(0)), 16));
console.log(show(bleed(seq(5)), 16));

// 4. A HOLE in the source. `delete` leaves `length` alone, so the read proof
//    still holds and the fast arm's own correction is what stores `undefined`.
const holed = seq(0);
delete holed[7];
const d3 = seq(100);
console.log(copy16(d3, holed, 'holed') + '|' + show(d3, 16));

// 5. A destination SHORTER than the run's largest index: the one length test
//    refuses, the whole span takes the ladder, and the ladder grows the array.
const short = [1, 2];
console.log(copy16(short, seq(0), 'short') + '|' + String(short.length) + '|' +
            show(short, 16));

// 6. A destination that is not an Array at all.
const plain = {};
console.log(copy16(plain, seq(0), 'plain') + '|' + show(plain, 16));

// 7. A SHIFTED destination: element zero is not slot zero, which is the head
//    test the store proof makes and the reason it is made in sixty-four bits.
const shifted = seq(0);
shifted.unshift(-1);
shifted.push(99);
shifted.shift();
console.log(copy16(shifted, seq(200), 'shifted') + '|' + show(shifted, 17));

// 8. A FROZEN destination. `Object.freeze` makes every element non-writable and
//    records the level in the array's named-properties side object, which is
//    the very thing the store proof tests for — so a frozen array can never
//    take the fast arm. 10.1.9.2 returns false and a sloppy reference discards.
const frozen = Object.freeze(seq(100));
console.log(copy16(frozen, seq(0), 'frozen') + '|' + show(frozen, 16));

// The same span through a strict reference throws at its first store, so
// nothing after it is written either.
const frozen2 = Object.freeze(seq(100));
let threw = '';
try {
  copy16Strict(frozen2, seq(0));
  threw = 'no-throw';
} catch (e) {
  threw = 'TypeError:' + String(e instanceof TypeError);
}
console.log(threw + '|' + show(frozen2, 16));

// 9. A SOURCE whose every index is an allocating getter: the read proof
//    refuses, the conjunction refuses with it, and the slow arm collects
//    between its own members — so what it has already read has to be somewhere
//    the collector forwards.
const gsrc = {};
let reads = 0;
for (let g = 0; g < 16; g++) {
  (function (k) {
    Object.defineProperty(gsrc, String(k), {
      get: function () {
        reads = reads + 1;
        const junk = [];
        for (let j = 0; j < 30; j++) junk.push({ j: j });
        return k * 2;
      },
      configurable: true
    });
  })(g);
}
const d4 = seq(100);
console.log(copy16(d4, gsrc, 'getters') + '|' + String(reads) + '|' + show(d4, 16));

// 10. A DESTINATION whose every index is a counting, allocating setter. The
//     count is how many stores the slow arm made: sixteen, one per member, and
//     everything it wrote before the span ended is still written.
const sink = {};
let writes = 0;
const held = [];
for (let s = 0; s < 16; s++) {
  (function (k) {
    Object.defineProperty(sink, String(k), {
      set: function (v) {
        writes = writes + 1;
        const junk = [];
        for (let j = 0; j < 30; j++) junk.push({ j: j });
        held[k] = v + junk.length - 30;
      },
      get: function () { return held[k]; },
      configurable: true
    });
  })(s);
}
console.log(copy16(sink, seq(0), 'setters') + '|' + String(writes) + '|' +
            show(sink, 16));

// 11. The store-run-only span, on a good destination and on three bad ones.
console.log(show(fill16(seq(0), 3), 16));
console.log(show(fill16({}, 3), 16));
console.log(show(fill16([0, 0], 3), 16));
console.log(show(fill16(seq(0), '3'), 16));
console.log(show(fill16(seq(0), 'x'), 16));

// 12. Back to the shape the arms were written for, so the sites the lines above
//     made polymorphic are shown still taking it.
const d5 = seq(100);
console.log(copy16(d5, seq(0), 'again') + '|' + show(d5, 16));
console.log(show(bleed(seq(0)), 16));
console.log(show(fill16(seq(0), 3), 16));
