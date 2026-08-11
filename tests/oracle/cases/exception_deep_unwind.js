// The GC-stress case for unwinding (docs/0020 decision 2).
//
// A throw crosses eight call frames, each of which holds freshly allocated
// objects, arrays and strings in locals AND runs a `finally` on the way out
// that reads them back. Under `BRONZE_GC_STRESS=1` a collection happens at
// every allocation, so each of those reads is a check that the frame's GC
// root slots (docs/0006) are still linked, still describe live values, and
// have been popped in the right order — a frame left on the chain shows up
// here as a crash or as a scrambled value, not as a missing line.
//
// Nothing here is about ECMA-262 beyond 14.15.3 (the finallys run, innermost
// first) and 14.14 (the thrown object is the same object at the catch).

function makeNode(depth) {
  return { depth: depth, tag: "node-" + depth, payload: [depth, depth * 2, depth * 3] };
}

// Allocation pressure between the interesting allocations, so the values the
// finallys read are not the youngest things on the heap.
function churn(k) {
  var junk = [];
  for (var i = 0; i < k; i++) {
    junk.push({ i: i, s: "junk" + i });
  }
  return junk.length;
}

var log = [];
var churned = 0;

function descend(n) {
  var a = makeNode(n);
  churned = churned + churn(8);
  var b = makeNode(n + 100);
  var s = a.tag + "|" + b.tag;
  try {
    churned = churned + churn(8);
    if (n === 0) {
      throw { bottom: makeNode(999), rungs: [] };
    }
    return descend(n - 1);
  } finally {
    // Reads three locals allocated before the throw, after an arbitrary
    // number of collections in the frames below this one.
    churned = churned + churn(8);
    log.push(s + ":" + a.payload[2] + ":" + b.depth);
  }
}

var bottom = null;
try {
  descend(7);
} catch (e) {
  bottom = e;
  e.rungs.push("caught");
  log.push("caught " + e.bottom.tag);
}
console.log(log.length);
console.log(log.join("\n"));

// The thrown object survived the unwind and every collection during it.
console.log(bottom.bottom.payload.join(","), bottom.rungs.join(","));
console.log(churned);

// The heap is still usable afterwards: allocate again and read the old
// values back, which is what a frame left on the shadow stack would break.
console.log(churn(64), log[0], log[7]);
