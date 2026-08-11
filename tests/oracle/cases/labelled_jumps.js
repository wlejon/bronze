// Labelled statements, and the two jumps that can name one.
//
// Derived from ECMA-262 14.13 (Labelled Statements), 14.8 (break) and 14.9
// (continue):
//
// 1. `break L` and `continue L` complete with a break/continue completion
//    carrying the label L, which propagates outward until it reaches the
//    LabelledStatement whose label set contains L. So a `continue outer` from
//    two loops deep abandons the inner loop entirely and resumes the outer
//    loop's update; the rest of the outer body after the inner loop never
//    runs.
// 2. 14.9.1 restricts `continue` to a label whose statement is an ITERATION
//    statement, but 14.8 puts no such restriction on `break`: a label may
//    front a plain block, and `break L` inside it jumps to the end of that
//    block. That makes a labelled block the structured early-exit that a
//    function without one has to fake with a flag.
// 3. A duplicate label is an early error only when the labels NEST (14.13.1
//    checks the enclosing label set). Two sibling loops may each be `lp:`,
//    because the first label is out of scope by the time the second appears.
// 4. A switch is breakable but not iterable, so inside a switch inside a
//    labelled loop the three jumps mean three different things: unlabelled
//    `break` leaves the switch, `continue L` starts the loop's next
//    iteration, and `break L` leaves the loop.
// 5. `continue L` on a `while` goes to the loop's CONDITION, not past it —
//    the label changes which loop is resumed, never where in it.
let a = "";
grid: for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    if (j === 1) continue grid;
    a = a + i + j + "|";
  }
  a = a + "tail|";
}
console.log(a);

let s = "";
search: for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    if (i * 3 + j === 4) break search;
    s = s + (i * 3 + j) + ",";
  }
}
console.log(s);

function deep() {
  let hits = 0;
  a1: for (let i = 0; i < 2; i++) {
    b1: for (let j = 0; j < 2; j++) {
      for (let k = 0; k < 2; k++) {
        hits = hits + 1;
        if (k === 0 && j === 1) continue a1;
        if (i === 1 && j === 0 && k === 1) break a1;
      }
    }
  }
  return hits;
}
console.log(deep());

function classify(n) {
  let out = "start";
  done: {
    if (n < 0) {
      out = "neg";
      break done;
    }
    if (n === 0) {
      out = "zero";
      break done;
    }
    out = "pos";
  }
  return out + "!";
}
console.log(classify(-1));
console.log(classify(0));
console.log(classify(5));

function firstNegative(arr) {
  let found = 0;
  scan: {
    for (let i = 0; i < arr.length; i++) {
      if (arr[i] < 0) {
        found = i;
        break scan;
      }
    }
    found = -1;
  }
  return found;
}
console.log(firstNegative([1, 2, -3, 4]));
console.log(firstNegative([1, 2, 3]));

let c = "";
lp: for (let i = 0; i < 3; i++) {
  if (i === 1) break lp;
  c = c + "A";
}
lp: for (let i = 0; i < 3; i++) {
  if (i === 1) break lp;
  c = c + "B";
}
console.log(c);

let b = "";
rows: for (let i = 0; i < 4; i++) {
  switch (i) {
    case 0:
      b = b + "z";
      break;
    case 2:
      continue rows;
    case 3:
      break rows;
    default:
      b = b + "d";
  }
  b = b + ".";
}
console.log(b);

function whileLabel() {
  let i = 0;
  let out = "";
  w: while (i < 5) {
    i = i + 1;
    if (i % 2 === 0) continue w;
    out = out + i;
  }
  return out;
}
console.log(whileLabel());

// A label naming the loop it is directly on is legal and means what the
// unlabelled jump would: the loop is its own innermost iteration statement.
function self() {
  let t = 0;
  only: for (let i = 0; i < 5; i++) {
    if (i === 3) break only;
    if (i === 1) continue only;
    t = t + i;
  }
  return t;
}
console.log(self());

// Labels do not cross a function boundary: `inner` is free to reuse a name
// that an enclosing function's label already holds.
function outerFn() {
  let r = "";
  same: for (let i = 0; i < 2; i++) {
    r = r + inner(i);
    if (i === 1) break same;
  }
  return r;
}
function inner(n) {
  same: for (let j = 0; j < 3; j++) {
    if (j === n) return "<" + n + ":" + j + ">";
    continue same;
  }
  return "<none>";
}
console.log(outerFn());
