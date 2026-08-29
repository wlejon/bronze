// A HOLE INSIDE A RUN OF ADJACENT CONSTANT-INDEX READS.
//
// `tests/oracle/cases/array_holes.js` pins what a hole means to the language.
// This case pins the one place the compiler answers that question without
// consulting the property cache: a receiver proof (src/codegen-llvm/
// llvm_recv_proof.h) proves one array dense and long enough ONCE, and the
// sixteen reads after it are a load each. A hole is the only answer such a
// load cannot give for itself — the bits in a hole slot are an internal tag
// and never a Value the program can see — so the correction from those bits to
// `undefined` is the whole of what the proof owes, and it is owed however far
// the correction has been moved from the load.
//
// Sixteen reads off one array, feeding arithmetic, is the shape that builds a
// run and then a guarded numeric region over it (src/lower/guard_region.h), so
// each read here is consumed twice: by a numberness guard, which must answer
// false for a hole exactly as it does for the `undefined` a hole reads as, and
// by the boxed value the region's slow copy is entered with, which must BE
// that `undefined`. Getting either wrong is silent: the arithmetic below would
// keep producing a number where the language says NaN.
//
// What is pinned, from ECMA-262 10.4.2.1 (Array [[Get]]), 7.3.12
// (HasProperty), 7.1.4 (ToNumber) and 6.1.6.1:
//
//   1. A hole in the middle of a run reads as `undefined`, and `typeof` says
//      so, while `in` says the index is not an own key.
//   2. Arithmetic over a run containing one is NaN, and stays NaN however many
//      present elements follow.
//   3. Filling the hole back in makes the run numeric again, with no trace of
//      the hole left in the answer.
//   4. The values a raw load CAN give are unchanged by any of this: -0 stays
//      negative zero (1/x is -Infinity, not +Infinity), NaN stays NaN, and a
//      non-number element reads back as itself.
//   5. A frozen array — one whose elements can no longer move or change — is
//      read by a run exactly as an open one is.

function sumRun(e) {
  const a0 = e[0], a1 = e[1], a2 = e[2], a3 = e[3];
  const a4 = e[4], a5 = e[5], a6 = e[6], a7 = e[7];
  const a8 = e[8], a9 = e[9], a10 = e[10], a11 = e[11];
  const a12 = e[12], a13 = e[13], a14 = e[14], a15 = e[15];
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 +
         a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15;
}

function readRun(e) {
  const a0 = e[0], a1 = e[1], a2 = e[2], a3 = e[3];
  const a4 = e[4], a5 = e[5], a6 = e[6], a7 = e[7];
  const a8 = e[8], a9 = e[9], a10 = e[10], a11 = e[11];
  const a12 = e[12], a13 = e[13], a14 = e[14], a15 = e[15];
  return [a0, a1, a2, a3, a4, a5, a6, a7,
          a8, a9, a10, a11, a12, a13, a14, a15];
}

const e = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];

// 0 + 1 + ... + 15
console.log('dense sum ' + sumRun(e));
console.log('dense read ' + readRun(e).join(','));

delete e[3];
delete e[10];
console.log('length ' + e.length);
console.log('hole read ' + String(e[3]) + ' ' + typeof e[3]);
console.log('hole in 3 ' + (3 in e) + ' in 2 ' + (2 in e));
console.log('hole own ' + Object.prototype.hasOwnProperty.call(e, 3));
console.log('hole sum ' + sumRun(e));
console.log('hole read run ' + readRun(e).join(','));

e[3] = 30;
e[10] = 100;
console.log('refilled sum ' + sumRun(e));
console.log('refilled read ' + readRun(e).join(','));

// A raw load's own answers, unchanged: negative zero keeps its sign, NaN
// keeps its identity, and an element that is not a number reads back as
// itself rather than as anything a numeric path might have made of it.
e[0] = -0;
e[1] = NaN;
e[2] = 'two';
const back = readRun(e);
console.log('minus zero ' + back[0] + ' ' + (1 / back[0]) + ' ' + Object.is(back[0], -0));
console.log('nan ' + back[1] + ' ' + (back[1] !== back[1]));
console.log('string ' + back[2] + ' ' + typeof back[2]);
console.log('mixed sum ' + sumRun(e));

// A frozen array is read by a run exactly as an open one is: freezing changes
// what a WRITE may do, and nothing about where an element lives.
const f = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
Object.freeze(f);
console.log('frozen ' + Object.isFrozen(f) + ' sum ' + sumRun(f));
console.log('frozen read ' + readRun(f).join(','));
