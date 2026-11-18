// Hot loops entered by OSR and left by each kind of exit, the frame going on
// after the loop in the interpreter with the values the loop made.

// Falls out of its condition; what follows reads values from before and
// from inside the loop.
function plain() {
  const label = 'plain';
  let s = 0, last = 0;
  for (let i = 0; i < 200000; i++) { s += i & 7; last = i; }
  return label + ' ' + s + ' ' + last;
}
console.log(plain());

// break from the middle, two different breaks, one taken.
function breaks(stopAt) {
  let s = 0, why = 'none', i = 0;
  for (; i < 1e9; i++) {
    if (i === stopAt) { why = 'stop'; break; }
    if (s > 1e12) { why = 'big'; break; }
    s += i % 13;
  }
  return why + ' ' + i + ' ' + s;
}
console.log(breaks(150000));

// return from inside: the frame returns, no exit.
function find(n) {
  for (let i = 0; ; i++) if (i * i > n) return i;
}
console.log(find(1e11));

// Nested: the inner loop goes hot inside the outer, exits back into it
// many times.
function nested() {
  let total = 0;
  for (let j = 0; j < 30; j++) {
    let row = 0;
    for (let i = 0; i < 20000; i++) row += (i ^ j) & 3;
    total += row;
  }
  return total;
}
console.log(nested());

// A labelled continue to the outer loop out of the hot inner one.
function labelled() {
  let hits = 0;
  outer: for (let j = 0; j < 40; j++) {
    for (let i = 0; i < 10000; i++) {
      if (i === 5000 + j) { hits += i; continue outer; }
    }
    hits = -1;
  }
  return hits;
}
console.log(labelled());

// Leaves the loop by a throw it catches after it.
function throws() {
  let i = 0;
  try {
    for (;; i++) if (i === 120000) throw new Error('at ' + i);
  } catch (e) {
    return e.message + ' ' + i;
  }
}
console.log(throws());

// Objects and strings made in the loop and read after it.
function objects() {
  let o = { n: 0 }, s = '';
  for (let i = 0; i < 100000; i++) {
    o = { n: o.n + 1, prev: i };
    if (i % 25000 === 0) s += i + ',';
  }
  return o.n + ' ' + o.prev + ' ' + s;
}
console.log(objects());

// Two hot loops one after another at top level, and code after both.
let a = 0;
for (let i = 0; i < 300000; i++) a += i & 1;
let b = 1;
for (let i = 0; i < 300000; i++) b = (b * 31 + i) % 1000003;
const after = [a, b].join(':');
console.log(after);
