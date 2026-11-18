// Code a harvested run never entered is laid out cold, after the main line
// (opt/speculate never_entered.cpp unenteredBlocks), and the copies a guard
// falls back to after the hot functions (codegen-brass function_placement.h).
// Trained on one arm and run on the other (run.py's switch mode; the other
// modes run both): the cold side of each branch below does typed work,
// calls out, allocates enough to collect while its frame is live, and
// throws through a caller; every answer must be what it is without the
// layout.
const TRAINING = Math.random() < 0.5;

function mix(a, b) { return ((a * 31) ^ b) & 0xffff; }

function walk(n, rare) {
  let h = 7;
  for (let i = 0; i < n; i++) {
    h = mix(h, i);
    if (rare && i % 5 === 0) {
      // Typed work only: nothing here counts in a harvested build.
      let t = i | 0;
      t = (t * 3 + 1) | 0;
      t = (t ^ (t >>> 3)) | 0;
      h = (h + t) & 0xffff;
    }
  }
  return h;
}

class Cell { constructor(v) { this.v = v; this.next = null; } }

// The rare side allocates a list long enough to need a collection, keeps it
// live across calls, and reads it back.
function build(n, rare) {
  let s = 0;
  for (let i = 0; i < n; i++) s += i & 3;
  if (rare) {
    let head = null;
    for (let i = 0; i < 20000; i++) {
      const c = new Cell(i);
      c.next = head;
      head = c;
      if ((i & 1023) === 0) s += mix(i, s) & 1;
    }
    let k = 0;
    for (let c = head; c !== null; c = c.next) k += c.v & 1;
    s += k;
  }
  return s;
}

// The rare side throws; the caller catches it.
function check(x, rare) {
  if (rare && x > 3) {
    const why = 'bad ' + x;
    throw new RangeError(why);
  }
  return x + 1;
}
function guarded(xs, rare) {
  let s = 0, caught = 0;
  for (const x of xs) {
    try {
      s += check(x, rare);
    } catch (e) {
      caught++;
      s += e.message.length;
    }
  }
  return s + ':' + caught;
}

function train() {
  let s = 0;
  for (let r = 0; r < 3000; r++) {
    s += walk(40, false) + build(20, false);
    s = (s + check(r & 3, false)) & 0xffffff;
  }
  return s + ' ' + guarded([1, 2, 3, 4, 5, 6], false);
}

function real() {
  let s = 0;
  for (let r = 0; r < 50; r++) s = (s + walk(40, true) + build(20, r === 7)) & 0xffffff;
  return s + ' ' + guarded([1, 2, 3, 4, 5, 6], true);
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(train());
} else {
  console.log(train());
  console.log(real());
}
