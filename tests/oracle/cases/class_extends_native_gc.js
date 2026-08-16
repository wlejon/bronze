// Construction of native-base subclasses in a LOOP, with an allocation between
// every step of it.
//
// The reason this is its own case: allocating a derived instance is a chain,
// not an allocation. The exotic object comes first, then the property box that
// carries the subclass's [[Prototype]], then the constructor body's own
// objects, then the private-field table — and under a moving collector every
// link after the first can relocate the ones before it. A value held raw across
// any of them is a pre-collection address, and the failure is silent: the read
// lands in a recycled block and answers a plausible number.
//
// So every line below reads a field of an object that was allocated BEFORE some
// later allocation and survived it, and the loop is long enough that a
// collect-on-every-allocation run moves each of them many times.
//
// `doubled instanceof Row` is the same question asked of the species path,
// which allocates the result array through a constructor call and then fills it
// — the longest chain here.

class Bag extends Map {
  #id;
  constructor(id) {
    super([["self", id]]);
    this.#id = id;
    this.tags = [];
  }
  get id() {
    return this.#id;
  }
  tag(t) {
    this.tags.push(t + this.#id);
    return this;
  }
}

class Row extends Array {
  constructor(n) {
    super();
    for (let i = 0; i < n; i++) this.push({ i: i, s: "s" + i });
  }
}

let total = 0;
let last = null;
for (let i = 0; i < 200; i++) {
  const b = new Bag(i);
  b.tag("t").tag("u");
  const r = new Row(3);
  const doubled = r.map((o) => ({ i: o.i * 2 }));
  total += b.size + b.tags.length + r.length + doubled.length + (doubled instanceof Row ? 1 : 0);
  if (i === 199) last = { b: b, r: r, doubled: doubled };
}
console.log(total);
console.log(last.b.size, last.b.get("self"), last.b.id, last.b.tags.join(","));
console.log(last.r.length, last.r[2].s, last.doubled[2].i, last.doubled instanceof Row);
console.log(last.b instanceof Bag, last.b instanceof Map, Array.isArray(last.r));

class Ring extends Set {
  constructor(n) {
    super();
    for (let i = 0; i < n; i++) this.add("k" + (i % 4));
  }
}
let sizes = 0;
let lastRing = null;
for (let i = 0; i < 200; i++) {
  const g = new Ring(9);
  g.note = "n" + i;
  sizes += g.size;
  lastRing = g;
}
console.log(sizes, lastRing.size, lastRing.note, [...lastRing].join(","));

// A promise chain long enough that every link's capability record outlives
// several collections, with a fresh object allocated inside each reaction.
class P2 extends Promise {}
let chain = P2.resolve(0);
for (let i = 0; i < 50; i++) {
  chain = chain.then((v) => {
    const o = { v: v, pad: [1, 2, 3, 4] };
    return o.v + o.pad.length - 3;
  });
}
chain.then((v) => console.log("chain", v, chain instanceof P2));
