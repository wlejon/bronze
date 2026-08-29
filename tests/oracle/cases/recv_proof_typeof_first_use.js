// A RUN OF PROVEN ELEMENT READS SPANNING THE PROCESS'S FIRST `typeof`.
//
// A receiver proof (src/codegen-llvm/llvm_recv_proof.h) proves one array once
// and reads its members through a DERIVED pointer, which is legal only while
// nothing between the reads can move the heap — `il::canCollect` is the
// oracle that bounds the run. `typeof` reads a tag and answers with one of
// eleven immortal strings, and it still collects: the table of answers is
// interned on the FIRST `typeof` the process runs, and interning allocates.
// This case puts that first `typeof` between the second and third read of a
// run. Under the gc-stress run the interning moves the array, and a run that
// kept its derived pointer across it reads the two later elements from the
// moved-from copy — `undefined`, silently, where the language says an object.
//
// Nothing else in this program spells `typeof`, so the one below is the
// first; keep it that way.

function runAcrossTypeof(arr) {
  const a0 = arr[0];
  const a1 = arr[1];
  const kind = typeof a0;
  const a2 = arr[2];
  const a3 = arr[3];
  return kind + ' ' + a0.n + a1.n + a2.n + a3.n;
}

console.log(runAcrossTypeof([{ n: 1 }, { n: 2 }, { n: 3 }, { n: 4 }]));
console.log(runAcrossTypeof([{ n: 5 }, { n: 6 }, { n: 7 }, { n: 8 }]));
