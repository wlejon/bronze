// `new Registry[name]()` — a constructor named by data, which three.js writes
// four times over (`new Curves[type]()` and friends).
//
// The trap this case pins is the granularity. A callee expression the analysis
// cannot name must not cost EVERY constructor in the program its parameters:
// the classes such a site can reach are the ones whose binding was read as a
// VALUE somewhere, because that read is how a constructor gets into a table in
// the first place. Those give up their parameters. A class that never leaves
// `new` position does not, however many computed constructions the program
// contains — and the computed site itself still contributes its ARGUMENTS to
// everything it might reach, which for a zero-argument `new` is `undefined`,
// which a defaulted parameter absorbs.
//
// `new this.constructor()` is the same shape reached through an instance, and
// three.js's `clone()` is written that way on half its classes.

class Arc {
  constructor(r = 1) {
    this.r = r;
  }
}

class Line {
  constructor(r = 2) {
    this.r = r;
  }
}

// Reading `Arc` and `Line` here is what puts them in reach of the computed
// `new` below, and it is the read — not the `new` — that stands them down.
const Registry = { Arc: Arc, Line: Line };

function make(name) {
  return new Registry[name]();
}

// Never read as a value, so nothing the program does with `Registry` can name
// it: its parameter keeps the join over the sites written out below.
class Solo {
  constructor(s = 3) {
    this.s = s;
  }
  clone() {
    return new this.constructor();
  }
}

function radius(c) {
  return c.r;
}

function scale(v) {
  return v.s * 2;
}

console.log("arc=" + make("Arc").r + " line=" + make("Line").r);
console.log("direct=" + radius(new Arc()) + "," + radius(new Arc(9)));

const solo = new Solo(5);
console.log("solo=" + scale(solo) + " " + solo.s);
// A value out of a computed `new` has no identity to read a field through, so
// this read is the boxed one — on an object whose slots the raw path filled.
console.log("cloned=" + solo.clone().s + "," + new Solo(6).clone().s);
console.log("default=" + new Solo().s);

solo.s = 1.5;
console.log("mutated=" + scale(solo));

let total = 0;
for (let i = 0; i < 50; i++) {
  total = total + scale(new Solo(i)) + make("Arc").r;
}
console.log("loop=" + total);

// The string a computed `new` names can be anything, including a name that is
// not in the table at all.
try {
  make("Missing");
} catch (e) {
  console.log("missing: " + (e instanceof TypeError));
}

console.log("keys=" + Object.keys(new Solo(2)).join(",") + "/" +
  Object.keys(new Arc(2)).join(","));
console.log("json=" + JSON.stringify(new Solo(4)) + JSON.stringify(make("Line")));
