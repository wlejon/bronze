// Loop index facts (opt/redundancy/index_facts.h): an int32 index that never
// goes below zero, under its `i < n`, needs no bounds test and its
// increment no overflow test. Every loop below is hot so the build on the
// harvest speculates it; the ones whose index can be negative, steps by
// more than one, or is compared the other way round must keep their tests.

function sumUp(a) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i];
  return s;
}

// Starts below zero: a[-3] .. a[-1] are undefined, and s goes NaN.
function fromNegative(a, start) {
  let s = 0;
  for (let i = start; i < a.length; i++) s += a[i] === undefined ? 1000 : a[i];
  return s;
}

// Steps by two: i + 2 under i < n may pass n.
function byTwo(a) {
  let s = 0, last = -1;
  for (let i = 0; i < a.length; i += 2) { s += a[i]; last = i; }
  return s * 1000 + last;
}

// The index is moved back inside the body.
function backAndForth(a) {
  let s = 0, steps = 0;
  for (let i = 0; i < a.length; i++) {
    s += a[i];
    if (a[i] === 3 && steps === 0) { i -= 5; steps++; }
  }
  return s * 100 + steps;
}

// n > i rather than i < n, and a bound below the array's length.
function reversedTest(a, n) {
  let s = 0;
  for (let i = 0; n > i; i++) s += a[i] === undefined ? 7 : a[i];
  return s;
}

// The bound is another array's length: the index is below n, not below
// a.length.
function otherBound(a, b) {
  let s = 0;
  for (let i = 0; i < b.length; i++) s += a[i] === undefined ? 1 : a[i];
  return s;
}

// Counting up to the top of the int32 range.
function nearTop(n) {
  let c = 0, last = 0;
  for (let i = 2147483640; i < n; i++) { c++; last = i; }
  return c + ':' + last;
}

// A nested loop whose index starts from the outer one.
function nested(a) {
  let s = 0;
  for (let i = 0; i < a.length; i++) {
    for (let j = i; j < a.length; j++) s += a[j] - a[i];
  }
  return s;
}

const a = [];
for (let i = 0; i < 10; i++) a.push(i);
const short = [1, 2, 3];
let out = [];
for (let k = 0; k < 3000; k++) {
  out = [sumUp(a), fromNegative(a, k % 2 === 0 ? 0 : -3), byTwo(a), backAndForth(a), reversedTest(a, 12),
         otherBound(short, a), nearTop(2147483647), nested(a)];
}
console.log(out.join(' '));
console.log(fromNegative(a, -2), reversedTest(short, 5), otherBound(a, short), nearTop(-5), nearTop(2147483645));
