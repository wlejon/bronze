// Stores of references after which generated code skips the write barrier
// when the target is young: every one of these must still be found by the
// collector, whether the target is young (skipped) or old (barriered).

// Long-lived holders: allocated first and kept, so they are promoted by the
// collections the churn below triggers.
const holders = [];
for (let i = 0; i < 64; i++) holders.push({ slot: null, list: [], env: null });
const holderArray = new Array(64).fill(null);

function churn(n) {
  let junk = 0;
  for (let i = 0; i < n; i++) junk += [i, { i }].length;
  return junk;
}

let checksum = 0;
for (let round = 0; round < 40; round++) {
  churn(2000);  // promote the holders, age what was stored last round
  for (let i = 0; i < 64; i++) {
    const fresh = { round, i, tag: "r" + round + "i" + i };
    holders[i].slot = fresh;               // old target, young value: barriered
    holders[i].list[round & 7] = fresh;    // old array, young value
    holderArray[i] = { inner: fresh };     // young wrapper into an old array
    const wrap = { a: fresh, b: [fresh] }; // young target, young values: skipped
    holders[i].env = () => wrap;           // a closure environment holding young
  }
  churn(3000);  // collections with the young values reachable only via holders
  for (let i = 0; i < 64; i++) {
    const h = holders[i];
    if (h.slot.round !== round || h.slot.i !== i) throw new Error("slot lost " + round + "/" + i);
    if (h.list[round & 7] !== h.slot) throw new Error("list lost " + round + "/" + i);
    if (holderArray[i].inner !== h.slot) throw new Error("array lost " + round + "/" + i);
    const w = h.env();
    if (w.a !== h.slot || w.b[0] !== h.slot) throw new Error("env lost " + round + "/" + i);
    checksum += h.slot.tag.length;
  }
}
console.log("checksum", checksum);

// for-of over objects: the iteration record's `current` is a young target.
const objs = [];
for (let i = 0; i < 100; i++) objs.push({ v: i });
let total = 0;
for (let r = 0; r < 200; r++) {
  for (const o of objs) { total += o.v; if ((o.v & 15) === 0) churn(10); }
}
console.log("forof", total);
