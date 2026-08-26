// Stage R3, observability class (a): a call to a closure whose environment
// chain includes the record.
//
// `bump` is a sibling closure over `make`'s record, so stage E1 gives the call
// a direct edge and LLVM inlines it — and once it is inlined, its write to `n`
// is an ordinary store in `run`'s loop, which stage R3's region rewrites along
// with the caller's own reads. That is the case this file exists to pin: a
// register that stayed live while inlined code wrote the heap slot would answer
// the read AFTER the call with the value from BEFORE it.
//
// The two reads straddling the call are what makes the sequence observable at
// all; `score` reads the same slot twice with no loop, which is the other kind
// of region — a whole function body no call can see.
function make() {
  let n = 0;
  let touched = 0;

  function bump(k) {
    n = n + k;
    touched = touched + 1;
  }

  function run(iters) {
    let seen = 0;
    for (let i = 0; i < iters; i++) {
      seen = seen + n;
      bump(i & 3);
      seen = seen + n;
    }
    return seen;
  }

  function score() {
    return n * n + touched;
  }

  return { run: run, score: score };
}

const m = make();
console.log(m.run(6));
console.log(m.score());
