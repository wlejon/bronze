// `new F(...args)` must hand the constructor the same values the spread array
// held, including when allocating the INSTANCE moves them.
//
// `bronze_construct` allocates twice — the function's prototype and the
// instance — before it reads its argument block, and it is the only helper in
// the runtime that reads a block after allocating. So a block that is not
// rooted holds pre-collection addresses by the time the constructor is
// entered. `bronze_construct_spread` built one in a plain `std::vector`, and
// this program segfaulted under `BRONZE_GC_STRESS=1` (docs/0032 decision 6);
// the sibling `bronze_dynamic_call_spread` did not, only because a plain call
// reaches the callee without allocating.
//
// It is in the default suite rather than beside it because `oracle-gc-stress`
// runs every case here, and a rooting bug is an intermittent wrong answer
// rather than a reliable crash — four of the five found so far showed up on a
// late stress run and none on an ordinary one.
class Pair {
  constructor(a, b) {
    this.a = a;
    this.b = b;
  }
}
const two = [{ n: 1 }, { n: 2 }];
const p = new Pair(...two);
console.log(p.a.n, p.b.n);

// Three heap objects, read in an order the constructor chooses rather than
// the order the block was built in.
function Trip(x, y, z) {
  this.sum = z.n + x.n + y.n;
  this.text = x.t + y.t + z.t;
}
const three = [{ n: 3, t: "a" }, { n: 4, t: "b" }, { n: 5, t: "c" }];
const q = new Trip(...three);
console.log(q.sum);
console.log(q.text);

// Strings are heap objects too, and a forwarded string reports a garbage
// length rather than failing a tag check.
function Join(a, b) {
  this.v = a + "-" + b;
}
console.log(new Join(...["left", "right"]).v);

// A fixed prefix and a spread tail, so the block is assembled from two
// sources before the one allocation that can move either.
function Four(a, b, c, d) {
  this.v = a.n + b.n + c.n + d.n;
}
const tail = [{ n: 20 }, { n: 30 }];
console.log(new Four({ n: 5 }, { n: 10 }, ...tail).v);
