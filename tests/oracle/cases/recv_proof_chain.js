// A run of constant-index reads that spans a straight line of BLOCKS, and
// every way the assumption behind it can be false at run time.
//
// The guarded numeric region splits a block after every value it guards, so
// `chain` below — sixteen reads off one array with arithmetic on each and three
// named stores between them — reaches the backend as seventeen blocks of one
// read apiece. llvm_recv_proof.h proves the receiver once in the first of them
// and spends it in the rest, which is sound only because each block after the
// first has one predecessor and takes no parameters.
//
// What that proof claims is checked ONCE, in front of the run: object, ARRAY
// kind, length past index 15, elements object. Everything below makes one of
// those false, or makes it stop being true PART WAY THROUGH — the three stores
// are where a setter gets to run, and a setter can shrink the array, punch a
// hole in it, or grow it so hard that the block the proof's base pointer was
// derived into is not the block the array uses any more. Each of those is a
// line here, and the guard is what has to answer.

function chain(a, dst) {
  const s0 = a[0] * 1 + a[1] * 2 + a[2] * 3 + a[3] * 4;
  dst.p = s0;
  const s1 = a[4] * 1 + a[5] * 2 + a[6] * 3 + a[7] * 4;
  dst.q = s1;
  const s2 = a[8] * 1 + a[9] * 2 + a[10] * 3 + a[11] * 4;
  dst.r = s2;
  const s3 = a[12] * 1 + a[13] * 2 + a[14] * 3 + a[15] * 4;
  return String(s0) + '|' + String(s1) + '|' + String(s2) + '|' + String(s3);
}

function dense() {
  return [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
}

// 1. The happy path, twice: the first call installs the store sites' shapes and
//    the second takes the inline slot arm they now cache, which is the arm the
//    proof is carried across.
const kept = { p: 0, q: 0, r: 0 };
console.log(chain(dense(), kept));
console.log(chain(dense(), kept));

// 2. A FRESH target every call, so each of the three stores is a shape
//    transition. A transition allocates the object's next shape, so the proof
//    dies at each store and the reads after it prove again.
console.log(chain(dense(), {}));

// 3. Shorter than the run's largest index, either side of the `15 < length`
//    test the one ladder makes.
console.log(chain([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15], kept));
console.log(chain([1, 2, 3, 4], kept));
console.log(chain([], kept));

// 4. A hole in the middle. `delete` leaves `length` where it was, so the one
//    length test still passes and the HOLE SELECT is what answers.
const holed = dense();
delete holed[9];
console.log(chain(holed, kept));

// 5. The assumption made false PART WAY THROUGH the run, by a setter on the
//    first store. Everything before the store is computed against the array as
//    it was; everything after must see what the setter did.
const shrunk = dense();
const shrinker = { q: 0, r: 0 };
Object.defineProperty(shrinker, 'p', {
  set: function (v) { shrunk.length = 4; },
  get: function () { return 0; },
  configurable: true
});
console.log(chain(shrunk, shrinker));

// 6. The same, punching two holes instead — one in the second group of four and
//    one in the third, so both of the later groups have to answer for it.
const punched = dense();
const puncher = { q: 0, r: 0 };
Object.defineProperty(puncher, 'p', {
  set: function (v) { delete punched[5]; delete punched[10]; },
  get: function () { return 0; },
  configurable: true
});
console.log(chain(punched, puncher));

// 7. The sharpest one: a setter that GROWS the array far enough to reallocate
//    its element block. The proof's base pointer is derived INTO that block, so
//    after this setter it points at storage the array has stopped using. The
//    reads after the store must re-derive; the values they answer are the same
//    ones, which is the whole point — a stale base would answer something else.
const grown = dense();
const grower = { q: 0, r: 0 };
Object.defineProperty(grower, 'p', {
  set: function (v) { for (let i = 0; i < 200; i++) grown.push(999); },
  get: function () { return 0; },
  configurable: true
});
console.log(chain(grown, grower));

// 8. The store target IS the array the run is about. `a.p = 1` on an array is
//    the named store that cannot take a bare slot arm — it reaches the runtime
//    to make the side object that holds the name, and that allocates — so the
//    run ends at it whatever the chain says.
const selfTarget = dense();
console.log(chain(selfTarget, selfTarget));
console.log(String(selfTarget.p) + ',' + String(selfTarget.q) + ',' +
            String(selfTarget.r) + ',' + String(selfTarget.length));

// 9. Receivers that are not arrays at all, at the same sites: the proof is
//    refused and the per-access ladder answers each one.
console.log(chain({ 0: 1, 1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 6: 7, 7: 8, 8: 9, 9: 10,
                    10: 11, 11: 12, 12: 13, 13: 14, 14: 15, 15: 16 }, kept));
console.log(chain(new Float64Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]),
                  kept));
console.log(chain('abcdefghijklmnop', kept));

// 10. Indices reached through a PROTOTYPE rather than owned. Nothing here is an
//     array, so the run is refused; what this pins is that the ladder's proto
//     walk still answers where the fast arm never could.
const protoIndices = { 8: 9, 9: 10, 10: 11, 11: 12 };
const inherited = Object.create(protoIndices);
inherited[0] = 1; inherited[1] = 2; inherited[2] = 3; inherited[3] = 4;
inherited[4] = 5; inherited[5] = 6; inherited[6] = 7; inherited[7] = 8;
inherited[12] = 13; inherited[13] = 14; inherited[14] = 15; inherited[15] = 16;
console.log(chain(inherited, kept));

// 11. Back to a dense array, so the sites the ten lines above made polymorphic
//     are shown still answering the run they were written for.
console.log(chain(dense(), kept));
