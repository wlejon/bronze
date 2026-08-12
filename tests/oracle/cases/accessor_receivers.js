// Who `this` is inside a getter, and what the rest of the language does when it
// meets an accessor instead of a slot.
//
// `accessor_properties` pins that a getter runs on read; this case pins the
// RECEIVER, which is the part every path had to be taught separately. A
// property read has four different ways of reaching a function in bronze —
// the proto walk, a function's own statics object, `super.x`, and the
// key-by-key copy that object spread performs — and each of them had a
// natural implementation that passes the wrong `this`, or none at all. Each
// of those is a silent wrong answer rather than a crash, so each gets a line
// here.
//
// What this pins, from ECMA-262 6.2.5.5 (Get with a Receiver), 10.1.8.1,
// 13.3.7.3 (`super`), 15.7.14 and 7.3.25 (CopyDataProperties):
//
// 1. One accessor on a prototype, two instances: the getter sees the INSTANCE
// it was reached through, so `a` and `b` disagree, and a setter reached the
// same way writes to the instance rather than to the shared prototype —
// `b.double = 8` must leave `a.n` alone. 2. A `static` accessor's receiver is
// the CONSTRUCTOR. `Reg.self` is the discriminating read: it is `true` only if
// `this` is `Reg` itself, and it would be `false` for a `this` of the statics
// side-object bronze actually stores the property on. 3. `super.tag` starts its
// LOOKUP at the parent prototype but keeps the original receiver (13.3.7.3), so
// the base getter reads the derived instance's `x`. A plain read of the
// prototype would have given the prototype as `this` and `undefined` as the
// answer. 4. Object spread copies the getter's VALUE, once, as a data property
// (7.3.25 uses Get, not the descriptor): the probe counts one run for the
// spread and none for the reads of the copy afterwards. 5. `console.log` of an
// accessor names the halves and does NOT run them. The probe count is unchanged
// across the inspect, which is the assertion — the format itself is
// the inspect format's. 6. `delete` removes the PAIR, and the key can then be re-added
// as an ordinary data property, landing at the END of the enumeration. 7. A
// setter-only property reads as `undefined` rather than as the setter function,
// because 10.1.8.1 returns undefined when [[Get]] is absent.
class Cell {
  constructor(n) {
    this.n = n;
  }
  get double() {
    return this.n * 2;
  }
  set double(v) {
    this.n = v / 2;
  }
}
const a = new Cell(1);
const b = new Cell(10);
console.log(a.double);
console.log(b.double);
b.double = 8;
console.log(b.n);
console.log(a.n);

class Reg {
  static get self() {
    return this === Reg;
  }
  static get label() {
    return "n=" + this.count;
  }
  static set label(v) {
    this.count = v.length;
  }
}
Reg.count = 3;
console.log(Reg.self);
console.log(Reg.label);
Reg.label = "abcd";
console.log(Reg.count);
console.log(Reg.label);

class Base {
  constructor(x) {
    this.x = x;
  }
  get tag() {
    return "base:" + this.x;
  }
}
class Derived extends Base {
  constructor(x) {
    super(x);
  }
  get tag() {
    return "derived(" + super.tag + ")";
  }
}
const d = new Derived(7);
console.log(d.tag);

const probe = { runs: 0 };
const src = {
  plain: 1,
  get live() {
    probe.runs = probe.runs + 1;
    return 42;
  },
};
console.log(probe.runs);
const copy = { ...src };
console.log(probe.runs);
console.log(copy.live);
console.log(copy.live);
console.log(probe.runs);
console.log(Object.keys(copy).join(","));
console.log(src);
console.log(probe.runs);

const both = {
  get v() {
    return 1;
  },
  set v(x) {
    this.tail = x;
  },
  other: 2,
};
console.log(both);
console.log(delete both.v);
console.log(both.v);
console.log(Object.keys(both).join(","));
both.v = 9;
console.log(both.v);
console.log(Object.keys(both).join(","));

const wo = {
  set only(x) {
    this.seen = x;
  },
};
console.log(wo.only);
wo.only = 5;
console.log(wo.seen);
console.log(wo);
