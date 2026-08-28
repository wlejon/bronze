// A loop nest. Chunk 1 duplicates the INNERMOST loop only — a region properly
// containing an already-chosen one is refused, which is the whole of the growth
// budget — so the outer accumulator arrives at the inner loop boxed and is
// tested once per ENTRY to it, not once per iteration.
//
// The outer accumulator flips to a String half way through, so the inner
// region's entry guard has to fail on some entries and hold on others, in one
// compiled function. What that pins is the one-way rule at the region level:
// leaving the fast copy is per ENTRY, so a later entry with a Number again is
// back on the fast path.

function nest(o, rows, cols, flipRow) {
  let total = 0;
  for (let r = 0; r < rows; r++) {
    if (r === flipRow) {
      total = total + "|";
    }
    for (let c = 0; c < cols; c++) {
      total = total + o.a + o.b;
    }
  }
  return total;
}

const o = { a: 1, b: 2 };

// No flip: every entry to the inner loop is a Number.
console.log(nest(o, 4, 3, -1));
// The flip happens before the second row, so the first row's inner loop ran on
// the fast path and the rest concatenate.
console.log(nest(o, 3, 2, 1));
// The flip happens before the first row: nothing runs on the fast path.
console.log(nest(o, 2, 2, 0));
// Zero inner iterations: the inner region is entered and left immediately.
console.log(nest(o, 3, 0, 1));

// The inner loop over a property that stops being a Number, with the outer
// accumulator untouched: the two regions are independent.
function nestFlip(o, rows, cols, flipRow) {
  let total = 0;
  for (let r = 0; r < rows; r++) {
    if (r === flipRow) {
      o.a = "s";
    }
    for (let c = 0; c < cols; c++) {
      total = total + o.a;
    }
  }
  return total;
}
console.log(nestFlip({ a: 2 }, 3, 2, 1));
