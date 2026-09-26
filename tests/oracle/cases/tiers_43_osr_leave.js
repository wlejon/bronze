// A loop in OSR code calls a function whose body the OSR code carries; the
// callee then meets a second shape on every iteration. Its failures
// invalidate the OSR code, and the frame leaves it at the next backedge with
// the loop's values (an int, a double, a boolean, the last result) and
// finishes in a lower tier. Every tier must print the same thing.
function g(o) {
  return o.a * o.b;
}
const plain = { a: 3, b: 2 };
const other = { c: 1, a: 5, b: 7 };

function run(n, flipAt) {
  let count = 0;
  let sum = 0.5;
  let seen = false;
  let last = 0;
  for (let i = 0; i < n; i++) {
    const v = g(i >= flipAt ? other : plain);
    sum += v / 4;
    count += 1;
    if (v !== 6) seen = true;
    last = v;
  }
  return count + ' ' + sum + ' ' + seen + ' ' + last;
}

console.log(run(2000, Infinity));
console.log(run(200000, 150000));
console.log(run(100000, 0));
console.log(run(100000, 100000));
console.log(run(3, 1));
