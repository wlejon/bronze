// Guards hoisted out of the loops of INLINED callees (opt/inline splice.cpp
// carries each callee loop's entry point; opt/speculate loops.cpp answers it):
// a hoisted guard that fails on entry finishes the inlined call from its G's
// loop entry and the caller after the call, with the callee's locals and the
// caller's as they were. Trained on packed int arrays of one shape; run on
// holey, double and object receivers, receivers whose kind or shape the loop
// body (or a call in it) changes, two levels of inlining, and a callee whose
// loop entry state holds values computed before the loop in both frames
// (run.py's switch mode; the other modes run both arms).
const TRAINING = Math.random() < 0.5;

function indexOf(list, x) {
  for (let i = 0; i < list.length; i++) if (list[i] === x) return i;
  return -1;
}
function findIn(holder, x) {
  const pre = holder.tag + ':';
  return pre + (indexOf(holder.items, x) + 1);
}

// The loop body's call may change the array's kind (int -> double) or
// append to it: the guard hoisted for the loop's entry no longer covers
// later iterations, which must still test (or leave) per iteration.
let mutate = null;
function sumWith(list, k) {
  let s = 0, seen = 0;
  for (let i = 0; i < list.length; i++) {
    s += list[i] * k;
    seen++;
    if (mutate !== null) mutate(list, i);
  }
  return s + '/' + seen;
}
function outerSum(list, k) {
  const base = k * 10;
  const r = sumWith(list, k);
  return base + '|' + r + '|' + list.length;
}

// Two levels: total -> rowSum -> a loop over a row's fields.
function rowSum(row) {
  let t = 0;
  for (let j = 0; j < row.cells.length; j++) {
    const c = row.cells[j];
    t += c === undefined ? 100 : c.v;
  }
  return t;
}
function total(rows, scale) {
  let acc = scale;
  for (let i = 0; i < rows.length; i++) acc += rowSum(rows[i]) * scale;
  return acc;
}

// A loop whose entry state holds locals of the inlined frame computed
// before it, and a caller local used after the call.
function scan(o, lim) {
  const a = o.a, b = o.b, start = a.length > 3 ? 1 : 0;
  let m = b;
  for (let i = start; i < lim && i < a.length; i++) if (a[i] > m) m = a[i];
  return m * 2 + start;
}
function useScan(o, lim) {
  const before = o.b + lim;
  const r = scan(o, lim);
  return before + ',' + r + ',' + (o.a.length - lim);
}

function mkRows(n, w) {
  const rows = [];
  for (let i = 0; i < n; i++) {
    const cells = [];
    for (let j = 0; j < w; j++) cells.push({ v: i + j });
    rows.push({ cells });
  }
  return rows;
}

const ints = [];
for (let i = 0; i < 20; i++) ints.push(i * 2);
const holder = { tag: 't', items: ints };
const rows = mkRows(5, 6);
const so = { a: [3, 9, 1, 7, 5, 2], b: 4 };

function train() {
  let s = 0;
  for (let r = 0; r < 4000; r++) {
    s += findIn(holder, (r % 50) * 2).length;
    s += outerSum(ints, r & 3).length;
    s += total(rows, r & 7);
    s += useScan(so, (r & 7) + 1).length;
  }
  return s;
}

function real() {
  const out = [];
  // Fails on entry: holey, double, generic object, string receivers.
  const holey = [1, , 3, 4];
  out.push(findIn({ tag: 'h', items: holey }, 4), findIn({ tag: 'h', items: holey }, undefined));
  out.push(findIn({ tag: 'd', items: [0.5, 1.5, 2.5] }, 2.5));
  out.push(findIn({ tag: 'o', items: { length: 3, 0: 'x', 1: 'y', 2: 'z' } }, 'z'));
  out.push(findIn({ tag: 's', items: 'abcde' }, 'd'));
  out.push(findIn({ tag: 'p', items: new Proxy([7, 8, 9], {}) }, 9));
  // The kind changes in the loop's body.
  mutate = (list, i) => { if (i === 2) list[i + 1] = 0.25; };
  out.push(outerSum([1, 2, 3, 4, 5], 3));
  mutate = (list, i) => { if (i === 1) list.push(100); if (list.length > 8) list.length = 8; };
  out.push(outerSum([1, 2, 3], 2));
  mutate = (list, i) => { if (i === 0) { list.length = 2; } };
  out.push(outerSum([5, 6, 7, 8], 1));
  mutate = (list, i) => { if (i === 1) list[3] = '7'; };
  out.push(outerSum([1, 2, 3, 4, 5], 1));
  mutate = null;
  out.push(outerSum([1.5, 2.5], 2), outerSum(['a', 'b'], 1));
  // Two levels: a row whose cells are of another shape, holey, or change.
  const odd = mkRows(3, 4);
  odd[1].cells[2] = { w: 1, v: 0.5 };
  out.push(total(odd, 2));
  const hol = mkRows(2, 3);
  delete hol[1].cells[1];
  out.push(total(hol, 1));
  const getr = mkRows(2, 3);
  Object.defineProperty(getr[0].cells[0], 'v', { get() { getr[1].cells.push({ v: 50 }); return 7; } });
  out.push(total(getr, 1));
  out.push(total({ length: 2, 0: { cells: [{ v: 1 }] }, 1: { cells: { length: 1, 0: { v: 2 } } } }, 3));
  // The inlined frame's and the caller's locals at a failing entry.
  out.push(useScan({ a: [3, 9.5, 1, 7], b: 2 }, 4), useScan({ a: [3, , 1, 70, 2], b: 2 }, 5));
  out.push(useScan({ a: { length: 5, 1: 8, 2: 30, 3: 4, 4: 1 }, b: 1 }, 4), useScan({ b: 0, a: [1, 2, 3, 4, 5] }, 9));
  let extra = 0;
  for (let r = 0; r < 2000; r++) extra += findIn({ tag: 'x', items: r & 1 ? holey : ints }, 4).length;
  out.push(extra);
  return out.join(' ');
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(train());
} else {
  console.log(train());
  console.log(real());
}
