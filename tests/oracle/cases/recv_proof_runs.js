// Runs of adjacent constant-index reads off one array — the shape
// llvm_recv_proof.h proves a receiver once for, and every way that proof can be
// wrong.
//
// The point of each block below is a DIFFERENT refusal. A dense run is the
// happy path; the rest make the single length test, the kind test and the hole
// select answer for cases the per-access ladder used to answer one at a time.

function run4(a) {
  // Four adjacent reads, no instruction between them that can collect: one run.
  const x0 = a[0], x1 = a[1], x2 = a[2], x3 = a[3];
  return String(x0) + ',' + String(x1) + ',' + String(x2) + ',' + String(x3);
}

// 1. Dense.
console.log(run4([10, 20, 30, 40]));

// 2. Longer than the run needs: the one length test clears index 3 and the
//    tail is untouched.
console.log(run4([10, 20, 30, 40, 50, 60]));

// 3. SHORTER than the run's largest index. The proof must refuse for the whole
//    run rather than read past the end.
console.log(run4([10, 20]));
console.log(run4([]));

// 4. Exactly the length of the largest index — the off-by-one either side of
//    the `maxIndex < length` test.
console.log(run4([10, 20, 30]));

// 5. A hole in the middle. `delete` leaves the dense part with a gap, and a
//    read of it is not an own property.
const holed = [10, 20, 30, 40];
delete holed[2];
console.log(run4(holed));

// 6. The prototype-inherited index a hole would otherwise have to find is not
//    reachable from here: bronze REFUSES `Array.prototype[2] = ...` by name
//    ("an array answers its members beside the value"), which is what makes
//    the hole select above a complete answer rather than a guess. Nothing to
//    print — the refusal is the test, and it is a hard error, so it cannot be
//    one of these lines.

// 7. The same run position reached by receivers of four different kinds, so the
//    site is polymorphic and the proof is refused on three of the four.
console.log(run4({ 0: 'a', 1: 'b', 2: 'c', 3: 'd' }));
console.log(run4(new Float64Array([1.5, 2.5, 3.5, 4.5])));
console.log(run4('wxyz'));
console.log(run4([10, 20, 30, 40]));

// 8. A run broken in the middle by a call that SHRINKS the array. The call can
//    collect, so the run ends at it and the reads after it re-prove.
const shrinking = [10, 20, 30, 40];
function shrink() {
  shrinking.length = 2;
  return 0;
}
function brokenRun(a) {
  const x0 = a[0], x1 = a[1];
  const gap = shrink();
  const x2 = a[2], x3 = a[3];
  return String(x0) + ',' + String(x1) + ',' + String(gap) + ',' + String(x2) + ',' + String(x3);
}
console.log(brokenRun(shrinking));

// 9. A run broken by a call that makes the array SPARSE, which takes it out of
//    the dense representation the proof speaks for.
const sparsed = [10, 20, 30, 40];
function scatter() {
  sparsed[100000] = 1;
  return 0;
}
function scatterRun(a) {
  const x0 = a[0], x1 = a[1];
  const gap = scatter();
  const x2 = a[2], x3 = a[3];
  return String(x0) + ',' + String(x1) + ',' + String(gap) + ',' + String(x2) + ',' + String(x3);
}
console.log(scatterRun(sparsed));

// 10. A long run — sixteen adjacent reads, which is the three.js `Matrix4`
//     shape and the one LLVM threads into a single straight-line block.
function run16(a) {
  const v0 = a[0], v1 = a[1], v2 = a[2], v3 = a[3];
  const v4 = a[4], v5 = a[5], v6 = a[6], v7 = a[7];
  const v8 = a[8], v9 = a[9], v10 = a[10], v11 = a[11];
  const v12 = a[12], v13 = a[13], v14 = a[14], v15 = a[15];
  return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 +
         v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15;
}
const sixteen = [];
for (let i = 0; i < 16; i++) sixteen.push(i * 0.5);
console.log(run16(sixteen));
console.log(run16([1, 2, 3]));

// 11. Two runs over two different receivers, back to back: the second must not
//     inherit the first's proof.
function twoReceivers(a, b) {
  const a0 = a[0], a1 = a[1];
  const b0 = b[0], b1 = b[1];
  return String(a0) + ',' + String(a1) + '|' + String(b0) + ',' + String(b1);
}
console.log(twoReceivers([1, 2], [3, 4]));
console.log(twoReceivers([1, 2], [3]));
console.log(twoReceivers({ 0: 9 }, [3, 4]));

// 12. A shifted array: `shift` moves the ring head, and the proof derives
//     element zero's address from it.
const shifted = [0, 1, 2, 3, 4, 5];
shifted.shift();
shifted.shift();
console.log(run4(shifted));
