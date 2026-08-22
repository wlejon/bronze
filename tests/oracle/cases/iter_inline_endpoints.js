// The inline iterator OPEN and the inline Array-kind END-OF-WALK
// (codegen-llvm/llvm_iter.cpp): a for-of over an array now allocates its
// record and marks its own end without a helper, so every endpoint the helper
// used to own is pinned here — the empty walk, the hole, growth and shrink
// mid-walk, the abrupt exits, and the kinds the inline path must refuse.

// ---- the empty array: open, one end-step, nothing else --------------------
let hits = 0;
for (const v of []) hits++;
console.log("empty:" + hits);

// ---- a hole reads as undefined (23.1.5.1 reads with Get), never a skip ----
const holey = [1, , 3];
const seen = [];
for (const v of holey) seen.push(v);
console.log(seen.join("|"));

// ---- growth DURING the walk is visited: length is re-read per step --------
const grow = [1, 2];
const got = [];
for (const v of grow) {
  got.push(v);
  if (v === 1) grow.push(99);
}
console.log(got.join(","));

// ---- shrink DURING the walk ends it early ---------------------------------
const shrink = [1, 2, 3, 4, 5];
const kept = [];
for (const v of shrink) {
  kept.push(v);
  if (v === 2) shrink.length = 3;
}
console.log(kept.join(","));

// ---- break and early return: the abrupt exits close cleanly ---------------
function firstOver(arr, bound) {
  for (const v of arr) {
    if (v > bound) return v;
  }
  return -1;
}
console.log(firstOver([3, 8, 1, 12], 5));
console.log(firstOver([1, 2], 5));
let broke = "";
for (const v of ["a", "b", "c", "d"]) {
  if (v === "c") break;
  broke += v;
}
console.log(broke);

// ---- nested for-of over the same array: two live records ------------------
const pairs = [];
const xs = [1, 2, 3];
for (const x of xs) {
  for (const y of xs) pairs.push(x * 10 + y);
}
console.log(pairs.join(","));

// ---- many loops in sequence: the record allocation path, hammered ---------
let total = 0;
const small = [2, 4, 6];
for (let i = 0; i < 50000; i++) {
  for (const v of small) total += v;
}
console.log("total:" + total);

// ---- the kinds the inline open must refuse, still correct -----------------
const s = "héllo";
let sChars = "";
for (const c of s) sChars += "[" + c + "]";
console.log(sChars);
const m = new Map([["k1", 1], ["k2", 2]]);
const mSeen = [];
for (const [k, v] of m) mSeen.push(k + "=" + v);
console.log(mSeen.join(","));
const set = new Set([7, 8]);
const setSeen = [];
for (const v of set) setSeen.push(v);
console.log(setSeen.join(","));
function* gen() {
  yield "g1";
  yield "g2";
}
const gSeen = [];
for (const v of gen()) gSeen.push(v);
console.log(gSeen.join(","));

// ---- a for-of INSIDE a generator body: the suspending loop keeps its
// record across yields, whichever path allocated it ------------------------
function* walker(arr) {
  for (const v of arr) yield v * 2;
}
console.log([...walker([1, 2, 3])].join(","));

// ---- spread and destructuring open records too ----------------------------
const spread = [...[9, 8, 7]];
console.log(spread.join(","));
const [p, ...rest] = [1, 2, 3, 4];
console.log(p + ":" + rest.join(","));
