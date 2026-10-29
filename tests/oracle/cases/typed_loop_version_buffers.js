// Typed-loop versioning (src/lower/typed_loop_version.h): a loop over a view
// nothing proved is copied with typed element reads and writes, entered behind
// a test of the view's kind. The kind is ALL that test fixes. A call in the
// loop that detaches the buffer, shrinks or grows a resizable one, or
// allocates enough to collect and move it, must leave every later access
// seeing the view as it then is: an index past a detached or shrunk view reads
// `undefined` (so `+ 1` is NaN) and its store is skipped.

function run(arr, n, at, act) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    arr[i] += 1;
    sum += arr[i] * 2;
    if (i === at) act(arr);
  }
  return sum;
}

function list(view) {
  return Array.from(view).join(',');
}

function fresh(Kind) {
  return new Kind([1, 2, 3, 4, 5, 6, 7, 8]);
}

// Detached after the fourth element: 2+3+4+5, doubled, then NaN.
for (const Kind of [Float32Array, Float64Array, Int32Array]) {
  const arr = fresh(Kind);
  let moved = null;
  const sum = run(arr, 8, 3, (a) => {
    moved = a.buffer.transfer();
  });
  console.log(Kind.name, 'detach', sum, arr.length, list(new Kind(moved)));
}

// A length-tracking view shrunk to three elements after the second.
for (const Kind of [Float32Array, Float64Array]) {
  const bytes = Kind.BYTES_PER_ELEMENT;
  const rab = new ArrayBuffer(8 * bytes, { maxByteLength: 16 * bytes });
  const arr = new Kind(rab);
  arr.set([1, 2, 3, 4, 5, 6, 7, 8]);
  const sum = run(arr, 8, 1, () => rab.resize(3 * bytes));
  console.log(Kind.name, 'shrink', sum, arr.length, list(arr));
}

// Grown to 64 elements after the second: the two past the old end read 0.
for (const Kind of [Float32Array, Float64Array]) {
  const bytes = Kind.BYTES_PER_ELEMENT;
  const rab = new ArrayBuffer(8 * bytes, { maxByteLength: 64 * bytes });
  const arr = new Kind(rab);
  arr.set([1, 2, 3, 4, 5, 6, 7, 8]);
  const sum = run(arr, 10, 1, () => rab.resize(64 * bytes));
  console.log(Kind.name, 'grow', sum, arr.length, list(arr.subarray(0, 10)));
}

// Enough allocation in the middle to collect: the view's bytes may move.
for (const Kind of [Float32Array, Float64Array]) {
  const arr = fresh(Kind);
  const junk = [];
  const sum = run(arr, 8, 2, () => {
    for (let k = 0; k < 20000; k++) junk.push({ k });
  });
  console.log(Kind.name, 'collect', sum, list(arr), junk.length);
}

// A plain array and a kind the pass does not version take the original loop.
for (const make of [() => [1, 2, 3, 4, 5, 6, 7, 8], () => fresh(Uint8Array), () => fresh(Float32Array)]) {
  const arr = make();
  console.log(run(arr, 8, -1, null), list(arr));
}
