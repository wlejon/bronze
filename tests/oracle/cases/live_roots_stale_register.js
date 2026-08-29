// A value generated code carries in a REGISTER rather than reloading from its
// GC root slot is correct only where nothing between the two can move the heap
// (codegen-llvm/llvm_live_roots.h). Each shape below puts an allocation between
// a value's definition and its use, in a place the plan has to see: after a run
// of proven element reads, across a throw, inside a guarded region's slow copy,
// through a merged callee, and over a loop back edge. Under the gc-stress run
// every one of those allocations moves the whole live set, so a register the
// plan wrongly trusted reads as a moved-from object.

class Cell {
  constructor(n) {
    this.n = n;
  }
  bump(k) {
    return this.n + k;
  }
}

// A run of adjacent constant-index reads off one array, then three allocations
// of different shapes, then the objects that run produced.
function runThenAlloc(src) {
  const a0 = src[0];
  const a1 = src[1];
  const a2 = src[2];
  const a3 = src[3];
  const pad = { keep: a0 };
  const list = [a1, a2, a3];
  const text = "n=" + a0.n;
  return a0.n + a1.n + a2.n + a3.n + pad.keep.n + list[0].n + text.length;
}

// The same run, with `new` as the allocation.
function runThenNew(src) {
  const a0 = src[0];
  const made = new Cell(a0.n);
  return a0.n + made.n;
}

// The narrowest shape there is: ONE allocation between a value's last reload
// and its next use, with nothing else in between, so exactly one instruction's
// answer decides whether the second read is a stale pointer.
function oneAllocBetween(o) {
  // `o === null` is a bit compare and moves nothing, so the object literal
  // below is the only instruction between `o` arriving in a register and the
  // read that dereferences it.
  const isNull = o === null;
  const junk = {};
  junk.v = o.n;
  return isNull ? 0 : junk.v;
}

function raise(p) {
  throw new Error("boom " + p.n);
}

// The throw is the allocation: minting the Error object collects, and the
// handler reads two values the protected region never touches. `o` in
// particular is read on the exception edge and nowhere else, so nothing but
// that edge can be the reason it is in its slot when the Error is allocated.
function throughCatch(o, p) {
  let out = 0;
  try {
    raise(p);
  } catch (e) {
    out = o.n + p.n + e.message.length;
  }
  return out;
}

// A loop the guarded-region pass duplicates, entered with a value that is not a
// number, so the run leaves the fast copy through its trampoline and finishes
// in the slow copy — with an allocation in the body either way.
function mixedSum(vals, obj) {
  let t = 0;
  for (let i = 0; i < vals.length; i++) {
    const v = vals[i];
    if (typeof v === "number") {
      t = t + v;
    } else {
      t = t + v.length;
    }
    const churn = { i: i, o: obj };
    t = t + churn.o.n;
  }
  return t;
}

// Two direct method edges, whose callees are merged into this frame, with an
// allocation between them.
function throughMerged(c, d) {
  const x = c.bump(1);
  const junk = { a: x, b: d };
  const y = d.bump(2);
  return x + y + junk.b.n + c.n;
}

// A value defined before the loop and read on every iteration, across an
// allocation and a back edge.
function acrossBackedge(o, n) {
  let acc = 0;
  const hold = o;
  let i = 0;
  while (i < n) {
    const garbage = { i: i, s: "s" + i };
    acc = acc + hold.n + garbage.i;
    i = i + 1;
  }
  return acc + hold.n;
}

const src = [{ n: 1 }, { n: 2 }, { n: 3 }, { n: 4 }];
console.log(runThenAlloc(src));
console.log(runThenNew(src));
console.log(oneAllocBetween({ n: 6 }));
console.log(throughCatch({ n: 5 }, { n: 7 }));
console.log(mixedSum([1, 2, "abc", 4], { n: 10 }));
console.log(throughMerged(new Cell(3), new Cell(4)));
console.log(acrossBackedge({ n: 2 }, 5));

// The same shapes again, now with the objects reached only through the values
// under test, so a stale register is the only way to lose them.
let total = 0;
let r = 0;
while (r < 40) {
  total = total + runThenAlloc([{ n: r }, { n: r + 1 }, { n: r + 2 }, { n: r + 3 }]);
  total = total + oneAllocBetween({ n: r });
  total = total + throughCatch({ n: r }, { n: 1 });
  total = total + throughMerged(new Cell(r), new Cell(1));
  r = r + 1;
}
console.log(total);
