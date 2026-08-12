// A long straight-line block full of short-lived dynamic temporaries, which
// is the shape GC root slot reuse (docs/0033 decision 5) is built for: each
// intermediate dies almost immediately and its frame slot is handed to the
// next one.
//
// What must hold is that a value is still readable everywhere it is read. A
// slot released one instruction too early gives its next occupant's bytes to
// whoever reads the old value — and under `BRONZE_GC_STRESS=1`, which the
// oracle suite runs every case under, a value whose slot was reused before
// its last use reads as a forwarded header rather than as an object, which is
// the silent-wrong-answer half of docs/0031 decision 7.
//
// So every line below reads something built several allocations earlier, and
// the answers are all derivable from the source by hand.
function mk(n) {
  return { n: n, s: "v" + n };
}

// Temporaries that die immediately: `mk(i)` is dead the moment `.n` is read,
// so the pool should be reusing one or two slots for the whole loop.
let total = 0;
for (let i = 1; i <= 50; i = i + 1) {
  total = total + mk(i).n;
}
console.log(total);

// Values that must survive many later allocations. `first` is defined before
// 50 objects are built and read after all of them.
const first = mk(1);
let joined = "";
for (let i = 2; i <= 50; i = i + 1) {
  joined = joined + mk(i).s.length;
}
console.log(first.n, first.s, joined.length);

// Deep straight-line nesting in ONE block, so the live set really is deep
// rather than a long chain of dead temporaries: every operand is still needed
// when the next is built.
const a = mk(2);
const b = mk(3);
const c = mk(4);
const d = mk(5);
const e = mk(6);
const f = mk(7);
console.log(a.n + b.n + c.n + d.n + e.n + f.n);
console.log(a.s + b.s + c.s + d.s + e.s + f.s);

// A value defined in one block and read in another, which the scan must pin
// rather than pool: `carried` crosses the loop boundary in both directions.
let carried = mk(9);
for (let i = 0; i < 20; i = i + 1) {
  const fresh = mk(i);
  if (fresh.n > carried.n) {
    carried = fresh;
  }
}
console.log(carried.n, carried.s);

// Block arguments are uses too (docs/0033 decision 5): `picked` reaches the
// join through the branch, not through an operand list.
function pick(flag) {
  const left = mk(100);
  const right = mk(200);
  const picked = flag ? left : right;
  return picked.s + ":" + left.n + ":" + right.n;
}
console.log(pick(true));
console.log(pick(false));
