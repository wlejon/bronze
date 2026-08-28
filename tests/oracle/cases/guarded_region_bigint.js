// BigInts against a guarded numeric region. A BigInt's tag sits ABOVE the
// number range in the NaN box, so it fails `is.number` by construction and the
// slow copy runs 13.15.3's BigInt algorithm — including the two errors that
// algorithm alone can raise. The pass needs no `--assume-no-bigint` and no
// reachability scan for this: the guard is a branch, and the branch it does not
// take is the language.
//
// A loop whose accumulator is a BigInt LITERAL is refused before a block is
// copied — a `const.bigint` in the candidate closure is a static proof that the
// guard would fail every time — so this case also pins that a statically
// refused region still computes what it always did.

function sum(o, n) {
  let total = 0n;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
console.log(String(sum({ a: 1n, b: 2n }, 4)));
console.log(sum({ a: 1n, b: 2n }, 4) === 12n);

// A Number accumulator meeting a BigInt property: 13.15.3 step 3 refuses the
// mixed pair with a TypeError, on the first iteration, with nothing added.
function mixed(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
function caught(fn) {
  try {
    return String(fn());
  } catch (e) {
    if (e instanceof TypeError) return "TypeError";
    if (e instanceof RangeError) return "RangeError";
    return e.constructor.name;
  }
}
console.log(caught(() => mixed({ a: 1n, b: 2 }, 3)));
console.log(caught(() => mixed({ a: 1, b: 2n }, 3)));

// A property that turns into a BigInt part way through, so the guard holds for
// two iterations and then does not.
function flip(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    if (i === 2) {
      o.a = 5n;
    }
    total = total + o.a;
  }
  return total;
}
console.log(caught(() => flip({ a: 1 }, 4)));

// BigInt division by zero is a RangeError (6.1.6.2.5 BigInt::divide), which no
// Number operation raises.
function divide(o, n) {
  let total = 12n;
  for (let i = 0; i < n; i++) {
    total = total / o.d;
  }
  return total;
}
console.log(caught(() => divide({ d: 2n }, 2)));
console.log(caught(() => divide({ d: 0n }, 1)));

// The arithmetic a BigInt loop does get, unmixed and exact past 2**53.
function power(o, n) {
  let total = 1n;
  for (let i = 0; i < n; i++) {
    total = total * o.m;
  }
  return total;
}
console.log(String(power({ m: 3n }, 40)));
