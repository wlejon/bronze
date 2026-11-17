// Loop index facts (opt/redundancy/index_facts.h) trained on loops whose
// index stays in [0, a.length), then run with the index starting below
// zero, a bound past the array's end, an index moved back inside the body
// and a count up to the top of the int32 range (run.py's switch mode; the
// other modes run both arms). Only an index the code proves non-negative
// loses its tests, whatever the harvest saw.
const TRAINING = Math.random() < 0.5;

function sumFrom(a, start, n) {
  let s = 0;
  for (let i = start; i < n; i++) s += a[i] === undefined ? 1000 : a[i];
  return s;
}
function sumTo(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += a[i] === undefined ? 1000 : a[i];
  return s;
}
function skipBack(a, at) {
  let s = 0, back = 0;
  for (let i = 0; i < a.length; i++) {
    s += a[i] === undefined ? 1000 : a[i];
    if (i === at && back === 0) { i -= 4; back = 1; }
  }
  return s;
}
function upTo(lo, hi) {
  let c = 0, last = 0;
  for (let i = lo; i < hi; i++) { c++; last = i; }
  return c + ':' + last;
}

const a = [];
for (let i = 0; i < 16; i++) a.push(i * 3);

function train() {
  let s = 0;
  for (let r = 0; r < 4000; r++) {
    s += sumFrom(a, r & 3, a.length) + sumTo(a, a.length) + skipBack(a, 99);
    s += upTo(r & 7, 9).length;
  }
  return s;
}
function real() {
  const out = [];
  out.push(sumFrom(a, -5, a.length), sumFrom(a, -5, 20), sumTo(a, 20), sumTo(a, -3));
  out.push(skipBack(a, 2), skipBack(a, 0));
  out.push(upTo(2147483640, 2147483647), upTo(-3, 2), upTo(2147483646, 2147483647));
  for (let r = 0; r < 3000; r++) out[0] += sumFrom(a, (r & 3) - 2, a.length + (r & 1));
  return out.join(' ');
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(train());
} else {
  console.log(train());
  console.log(real());
}
