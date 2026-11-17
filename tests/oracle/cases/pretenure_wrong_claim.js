// A wrong pretenuring claim (docs/type-feedback.md "Pretenuring"): the
// training run keeps every object its literal and its constructor make, so
// the harvest pretenures both sites, and the real run drops nearly all of
// them at once. The collector's probes find them dying young and refuse the
// sites (brass heap_pretenure.cpp): from then on they allocate young, through
// the same call. Objects made old, made young as probes and made young after
// the refusal name one another and young arrays and strings, so every mix of
// generations a field store can meet is read back.
const TRAINING = Math.random() < 0.5;
if (TRAINING) console.error('TRAINING');

class Node {
  constructor(i, next) {
    this.i = i;
    this.tag = 't' + (i % 97);
    this.next = next;
    this.parts = [i, i + 1];
  }
}

function node(i, next) {
  return new Node(i, next);
}

function lit(i, prev) {
  return { i: i, prev: prev, parts: [i, 's' + (i % 89)], half: i / 2 };
}

const keep = [];
let sum = 0;
if (TRAINING) {
  for (let i = 0; i < 300000; i++) keep.push(lit(i, null), node(i, null));
} else {
  let prev = null;
  let n = null;
  for (let r = 0; r < 12; r++) {
    for (let i = 0; i < 50000; i++) {
      const o = lit(i, i % 100 === 0 ? null : prev);
      prev = o;
      n = node(i, i % 7 === 0 ? null : n);
      sum += o.parts[0] + o.parts[1].length + (o.prev === null ? 1 : o.prev.half) + n.parts[1] + n.tag.length;
      if (n.next !== null) sum += n.next.i;
      if (i % 5000 === 0) keep.push(o, n);
    }
  }
}
let check = 0;
for (const x of keep) {
  check += x.i;
  if (x instanceof Node) check += x.parts[1] + x.tag.length + (x.next === null ? 0 : x.next.parts[0]);
  else check += x.parts[1].length + (x.prev === null ? 0 : x.prev.parts[0]);
}
console.log(sum, keep.length, check);
