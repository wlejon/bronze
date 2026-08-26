// Stage R3's write-back discipline: every edge that leaves a region carries the
// write-back, and this file is one loop per KIND of edge.
//
//   `upto`        leaves through a `break`;
//   `untilReturn` leaves through a `return` from inside the loop, which is an
//                 exit edge like any other because a `return` block cannot
//                 reach the latch;
//   `untilThrow`  leaves through a `throw`, which in this runtime is a pending
//                 cell and a return rather than an unwind — so it leaves
//                 through the same kind of terminator the other two do.
//
// After each one, `get` reads the record through a different closure. An exit
// the analysis forgot is a stale heap slot read by someone with a legitimate
// view, and that is exactly what the second, fourth and sixth lines would show.
//
// `boom` is built once, outside the loops, because building it inside one would
// put an allocation in the region and the interesting question here is the
// EDGE, not what else the block contains.
function make() {
  const boom = new Error("stop");
  let n = 0;

  function upto(limit) {
    for (let i = 0; i < 100; i++) {
      n = n + 1;
      if (n >= limit) break;
    }
    return n;
  }

  function untilReturn(limit) {
    for (let i = 0; i < 100; i++) {
      n = n + 2;
      if (n >= limit) return i;
    }
    return -1;
  }

  function untilThrow(limit) {
    for (let i = 0; i < 100; i++) {
      n = n + 3;
      if (n >= limit) throw boom;
    }
    return -1;
  }

  return {
    upto: upto,
    untilReturn: untilReturn,
    untilThrow: untilThrow,
    get: function () { return n; }
  };
}

const m = make();
console.log(m.upto(5));
console.log(m.get());
console.log(m.untilReturn(12));
console.log(m.get());
try {
  m.untilThrow(20);
  console.log("no throw");
} catch (e) {
  console.log("caught " + e.message);
}
console.log(m.get());
