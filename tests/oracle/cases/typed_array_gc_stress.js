// A view's bytes live in an ArrayBuffer the collector MOVES, and a view holds
// that buffer as a Value plus a byte offset — never a cached data pointer. This
// case is the proof: it allocates thousands of views and buffers while holding
// older ones live, then reads every one of them back.
//
// Under the `oracle-gc-stress` run every single allocation below moves the
// entire live set, so a view whose buffer was not forwarded, or a runtime
// method that cached a raw pointer across an allocation, does not merely risk
// being wrong — it reads relocated memory on the very first churn iteration.
// Chunks 10 and 13 each shipped a rooting bug that only this suite caught.
const held = new Float32Array(4);
held[0] = 0.5;
held[3] = 2.25;

const heldBuffer = new ArrayBuffer(8);
const heldBytes = new Uint8Array(heldBuffer);
heldBytes[0] = 1;
heldBytes[7] = 2;

// An aliasing pair: the collector has to move ONE buffer and leave both
// views pointing at it, or the two stop agreeing.
const aliasA = new Uint16Array(heldBuffer, 2, 2);
const aliasB = new Uint8Array(heldBuffer, 2, 4);
aliasA[0] = 513; // 0x0201

let i = 0;
let checksum = 0;
while (i < 2000) {
  const churn = new Float32Array(3);
  churn[0] = i;
  churn[2] = i + 1;
  checksum = checksum + churn[0] + churn[2];
  i = i + 1;
}
console.log(checksum);
console.log(held[0] + held[3]);
console.log(heldBytes[0], heldBytes[7]);
console.log(aliasB[0], aliasB[1]);

// The same again with the churn allocating BUFFERS rather than views, so the
// RawBytes half of the heap is what moves.
let j = 0;
while (j < 2000) {
  const scratch = new ArrayBuffer(16);
  const view = new Int32Array(scratch);
  view[3] = j;
  j = j + 1;
}
console.log(held[3], heldBytes[7], aliasA[0]);

// A view that survives across a CALL, so its slot is a caller frame's rather
// than the top level's, and the callee allocates plenty on the way.
function churnAndSum(view) {
  let k = 0;
  let acc = 0;
  while (k < 500) {
    const tmp = new Uint8Array([k & 255, 1]);
    acc = acc + tmp[1];
    k = k + 1;
  }
  return acc + view[0] + view[3];
}
console.log(churnAndSum(held));

// Methods that ALLOCATE while walking a view: `map` builds a result and calls
// back into user code per element, and the callback here allocates too. The
// receiver, the result and the callback's own view all have to survive it.
const walked = new Int16Array([1, 2, 3, 4]);
const doubled = walked.map(function (x) {
  const noise = new Float64Array(2);
  noise[0] = x;
  return noise[0] * 2;
});
console.log(doubled);
console.log(walked);

// `slice` copies into a new buffer and `subarray` re-points at the old one;
// both survive a collection that moves the buffer underneath them.
const kept = new Uint8Array([9, 8, 7, 6]);
const keptSlice = kept.slice(1, 3);
const keptSub = kept.subarray(1, 3);
let m = 0;
while (m < 1000) {
  const junk = new Float32Array(8);
  junk[0] = m;
  m = m + 1;
}
kept[1] = 0;
console.log(keptSlice, keptSub);

// And the constructor objects themselves: they are interned function objects
// held by a runtime cache that has to be a GC root, not a raw pointer.
console.log(kept.constructor === Uint8Array, keptSlice.constructor === Uint8Array);
