// A suspension inside the body of a `for-of` or a `for-in`.
//
// These two loops are the only ones that carry state the SOURCE never named:
// an Iterator Record, opened once for the whole statement and stepped once per
// iteration. Every other loop's state is a binding, and a generator's frame
// holds bindings. So this case is about one question — does the record survive
// a suspension, and is it still the SAME record on the other side.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 14.7.5.6 ForIn/OfBodyEvaluation: one `iteratorRecord` for the statement,
//   step 5.a calling `next` on it once per trip. Nothing re-opens it, so two
//   trips of one loop see two successive elements however much ran in between.
// * 27.5.3.2 GeneratorResumeAbstractClosure: `next()` resumes the body at the
//   point it suspended. A loop in the middle of its second iteration continues
//   into its third rather than restarting.
// * 13.2.4.1 with 25.1.4.2: spread drains the iterator, so the array holds the
//   yielded values in order and NOT the generator's return value; that value
//   is only visible as the `value` of the final result.
// * 14.8.3 `break` leaves the loop with a break completion, and 14.7.5.6 step
//   5.j routes an abrupt one through 7.4.9 IteratorClose.
// * 27.5.3.3 `return(v)` at a suspension is a RETURN completion raised at the
//   suspension point, so 7.4.9 closes every iterator the point is inside,
//   innermost first — and the walk then ends with the value it was given.
// * 7.4.9 step 6: the value a `return` method answers is discarded; only its
//   abruptness could matter, and here there is none.
// * 14.7.5.6 for-in over 14.7.5.9 EnumerateObjectProperties: own enumerable
//   string keys, in property creation order for a plain object literal.

// --- one record per loop, two loops nested -------------------------------
function* pairs(xs, ys) {
  for (const x of xs) {
    for (const y of ys) {
      yield x + y;
    }
  }
  return 'end';
}

// Drained in one go: four values, and 'end' is not one of them.
console.log([...pairs(['a', 'b'], ['1', '2'])]);

// Walked by hand, so the return value is visible where 27.5.3.2 puts it — as
// the `value` of the first result that says done.
const walk = pairs(['a'], ['1', '2']);
console.log(walk.next(), walk.next(), walk.next(), walk.next());

// --- `break` out of a body that suspends ---------------------------------
// 1 yields, 2 yields, 3 > 2 breaks; the trailing yield is outside the loop.
function* upto(xs, limit) {
  for (const x of xs) {
    if (x > limit) break;
    yield x;
  }
  yield 'stopped';
}
console.log([...upto([1, 2, 3, 4], 2)]);

// --- `for-in`, whose subject is the key snapshot -------------------------
function* keysOf(o) {
  for (const k in o) {
    yield k;
  }
}
console.log([...keysOf({ p: 1, q: 2, r: 3 })]);

// --- IteratorClose from a suspension, innermost first --------------------
// An iterator that says when it is closed, so the ORDER of the two closes is
// observable rather than assumed.
function tracked(name, items) {
  let i = 0;
  return {
    [Symbol.iterator]: function () { return this; },
    next: function () {
      if (i < items.length) { return { value: items[i++], done: false }; }
      return { value: undefined, done: true };
    },
    return: function (v) {
      console.log('close ' + name);
      return { value: v, done: true };
    }
  };
}

function* nestedTracked() {
  for (const x of tracked('outer', ['a', 'b'])) {
    for (const y of tracked('inner', ['1', '2'])) {
      yield x + y;
    }
  }
}

const g = nestedTracked();
console.log(g.next().value);
console.log(g.return('R'));
