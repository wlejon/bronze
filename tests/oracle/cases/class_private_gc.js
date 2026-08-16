// Private state across collections.
//
// bronze's collector MOVES objects, and a private element is stored in a table
// keyed by the object that carries it — so every access rehashes against
// addresses the collector may have changed, and every step below allocates
// between the moment an object is created and the moment its private state is
// read. A missed root here is not a crash but a wrong ANSWER: a table entry
// found under a stale address, or a value read from a slot that moved.
//
// Under BRONZE_GC_STRESS=1 this collects at every allocation, so the loops are
// deliberately small and the arithmetic is deliberately checkable.

class Cell {
  #value;
  #tag;
  #history = [];

  constructor(i) {
    this.#value = i;
    this.#tag = "c" + i;
    this.#history.push(i);
  }

  add(n) {
    this.#value += n;
    this.#history.push(n);
    return this;
  }

  value() {
    return this.#value;
  }

  tag() {
    return this.#tag;
  }

  size() {
    return this.#history.length;
  }
}

const cells = [];
for (let i = 0; i < 40; i++) {
  const c = new Cell(i);
  // Allocation between construction and the private write below.
  const filler = { i: i, pad: [i, i * 2, i * 3], text: "pad" + i };
  c.add(filler.pad.length);
  cells.push(c);
}

let total = 0;
let tags = "";
for (let i = 0; i < cells.length; i++) {
  // And between the read of one cell and the read of the next.
  const more = { pad: [i, i + 1], text: "t" + i };
  total += cells[i].value() + cells[i].size() + more.pad.length;
  if (i % 13 === 0) {
    tags += cells[i].tag() + ";";
  }
}
console.log(total);
console.log(tags);

// A class EXPRESSION inside a loop: one evaluation per iteration, so one set of
// private names per iteration, and the tables they live in are heap objects the
// collector moves like any other.
const makers = [];
for (let i = 0; i < 5; i++) {
  const K = class {
    #n = i;
    static owns(o) {
      return #n in o;
    }
    read() {
      return this.#n;
    }
  };
  makers.push({ K: K, inst: new K() });
}

let owned = 0;
let cross = 0;
for (let a = 0; a < makers.length; a++) {
  const scratch = { a: a, list: [a, a, a] };
  for (let b = 0; b < makers.length; b++) {
    if (makers[a].K.owns(makers[b].inst)) {
      owned += scratch.list.length;
    } else {
      cross += 1;
    }
  }
}
console.log(owned, cross);

let readSum = 0;
for (const m of makers) {
  readSum += m.inst.read();
}
console.log(readSum);
