// The layout-family guard: one site in a base-class method, reached by every
// proven subclass — and by the shapes that must NOT be mistaken for one.
//
// A family site's claim is "the own properties of the shape in front of me
// BEGIN with my class's field list, at the same slots, with the same
// attributes". Every line below is a way for that to be false, or to stop being
// true, and each has to answer exactly what it answered before the claim
// existed.

class Base {
  constructor(n) {
    // A non-writable data property, installed the way three.js gives Object3D
    // its `id`. It is slot 0, so every subclass inherits it there — and a write
    // site may never claim it, because a fixed-offset store cannot refuse.
    Object.defineProperty(this, "id", { value: n });
    this.a = 1;
    this.b = 2;
  }
  read() {
    return this.a * 100 + this.b;
  }
  bump(v) {
    this.a = this.a + v;
    return this.a;
  }
  idOf() {
    return this.id;
  }
  poke() {
    // Whether this throws is a strictness question; whether `id` CHANGES is the
    // layout question, and that is the one being pinned.
    try {
      this.id = 999;
    } catch (e) {
      // 10.4.5 refuses the write; either way the value must stand.
    }
    return this.id;
  }
}

class Mid extends Base {
  constructor(n) {
    super(n);
    this.c = 3;
  }
  readC() {
    return this.c;
  }
}

class LeafA extends Mid {
  constructor(n) {
    super(n);
    this.d = 4;
  }
}

class LeafB extends Mid {
  constructor(n) {
    super(n);
    this.e = 5;
    this.f = 6;
  }
}

// A SIBLING branch: same base, same first three slots, and slot 3 is `g` where
// Mid's is `c`. A Mid site that accepted this shape would read 7 for `this.c`,
// which is the exact failure a range over the `extends` subtree exists to rule
// out.
class Sib extends Base {
  constructor(n) {
    super(n);
    this.g = 7;
  }
}

const all = [new Base(1), new Mid(2), new LeafA(3), new LeafB(4), new Sib(5)];

// Warm past the point where every cache, cell and stamp on the path is filled.
let warm = 0;
for (let i = 0; i < 500; i++) {
  for (let j = 0; j < all.length; j++) warm += all[j].read();
}
console.log(warm);

console.log(all.map((o) => o.idOf()).join(","));
console.log(all.map((o) => o.bump(10)).join(","));
console.log(all.map((o) => o.read()).join(","));
console.log(all.map((o) => o.poke()).join(","));

// The Mid site over the three classes that really are Mids.
console.log([all[1], all[2], all[3]].map((o) => o.readC()).join(","));
// ...and the sibling, whose slot 3 is `g`. `c` is absent there, so the answer
// is `undefined`; 7 would mean the range accepted a class outside it.
console.log(String(Mid.prototype.readC.call(all[4])));

// A foreign object with the right names in the right order, but whose `id` is
// an ordinary writable property: a data property with different ATTRIBUTES is a
// different layout, so the stamp refuses it and the site falls back — to the
// same answer.
const foreign = {};
foreign.id = 9;
foreign.a = 5;
foreign.b = 6;
console.log(Base.prototype.read.call(foreign));
// And one that is nothing like the layout at all.
console.log(Base.prototype.read.call({ b: 1, a: 2 }));

// An instance that gains a property is at a NEW shape, which no site has ever
// stamped. The guard misses until the runtime has verified that shape too, and
// the answers do not depend on which side of that it is on.
const grown = new LeafA(6);
grown.extra = 42;
console.log(grown.read() + "," + grown.readC() + "," + grown.extra);
let g = 0;
for (let i = 0; i < 300; i++) g += grown.read();
console.log(g);

// `delete` drops the object into dictionary mode, where slot numbering is a
// run-time fact and no stamp exists — so every family site misses, forever, and
// every read is still right.
const del = new LeafB(7);
delete del.e;
console.log(del.read() + "," + del.readC() + "," + String(del.e) + "," + del.f);
console.log(Object.keys(del).join(","));

// Freezing takes the same escape, and still has to answer reads.
const froz = new LeafA(8);
Object.freeze(froz);
console.log(froz.read() + "," + froz.readC() + "," + froz.idOf());

// A subclass this compilation never modelled: an anonymous class expression,
// which the layout analysis does not collect at all. Its instances still run
// the base constructors first, so their shape begins with Mid's fields — the
// runtime verifies that for itself and stamps them, and the inherited sites
// hit rather than being refused.
function makeSub() {
  return class extends Mid {
    constructor(n) {
      super(n);
      this.h = 8;
    }
  };
}
const Sub = makeSub();
const sub = new Sub(11);
console.log(sub.read() + "," + sub.readC() + "," + sub.idOf() + "," + sub.h);
let s2 = 0;
for (let i = 0; i < 300; i++) s2 += sub.read() + sub.readC();
console.log(s2);

// Enumeration order is insertion order, and a stamp is not an opinion about it.
console.log(Object.keys(all[3]).join(","));
console.log(JSON.stringify(all[2]));

// An accessor installed on the base prototype after every instance exists. An
// own property still shadows it; an object with no own properties at all sees
// the getter through two prototype links.
Object.defineProperty(Base.prototype, "b", {
  get() {
    return -1;
  },
  configurable: true,
});
console.log(all[0].read());
const bare = Object.create(Mid.prototype);
console.log(String(bare.readC()) + "," + bare.b);
