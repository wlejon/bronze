// `get x() {}` and `set x(v) {}` — a property that is a PAIR OF FUNCTIONS
// rather than a value in a slot.
//
// An object has a shape and a flat slot vector, and every read was
// `shape.lookupProperty` then a load. An accessor makes the read a CALL, which
// is why the shape node carries the kind and why the inline caches of an inline
// cache must refuse to describe one: a cached hit is an indexed load in
// generated code, and folding an accessor into that would return the getter
// function instead of running it (pinned separately by
// `inline_cache_shape_changes`).
//
// What this pins, from ECMA-262 6.1.7.1 (Property Attributes), 10.1.8.1,
// 10.1.9.2 and 15.7.14 (class accessors):
//
// 1. A getter runs on READ, at the site of the read, with `this` bound to the
// receiver — so `full` recomputes after `first` changes rather than holding a
// value captured at definition. 2. A setter runs on WRITE and its return value
// is discarded; the property afterwards is whatever the getter says, not what
// was assigned. Assigning "Ada Lovelace" therefore shows up as two separate
// fields. 3. An accessor with only a getter silently ignores a write in sloppy
// mode — `area` keeps computing from `side`. 10.1.9.2 returns false for it and
// a non-strict Set discards that; the TypeError is the STRICT-mode answer, and
// `cases/strict_mode` is where that half is pinned. The two cases hold the two
// halves of one rule (13.15.2 PutValue step 6.d) and neither is a special case
// of the other, which is why this file must keep printing 9 after the write:
// the flag rides on the write INSTRUCTION, so one program can hold both.
// 4. An accessor defined in an object literal is
// ENUMERABLE, so it appears in `Object.keys` and its getter runs when the key
// is read back. A class accessor is not: 15.7.14 defines it with `enumerable:
// false`, the same rule that already keeps class methods out of enumeration. 5.
// An accessor on a PROTOTYPE is found by the ordinary proto walk and runs with
// `this` as the instance, not as the prototype — which is what makes a getter
// usable as a computed field on every instance at once. `c.r` is an own data
// property and `diameter` is not, so the two `Object.keys` results below differ
// in exactly that. 6. `get x` and `set x` are ONE property with two halves, not
// two properties: `person` has three keys, not four.
const person = {
  first: "Ada",
  last: "L",
  get full() {
    return this.first + " " + this.last;
  },
  set full(v) {
    const sp = v.indexOf(" ");
    this.first = v.slice(0, sp);
    this.last = v.slice(sp + 1);
  },
};
console.log(person.full);
person.first = "Grace";
console.log(person.full);
person.full = "Ada Lovelace";
console.log(person.first);
console.log(person.last);
console.log(person.full);
console.log(Object.keys(person).join(","));

const square = {
  side: 3,
  get area() {
    return this.side * this.side;
  },
};
console.log(square.area);
square.area = 100;
console.log(square.area);
square.side = 5;
console.log(square.area);

class Circle {
  constructor(r) {
    this.r = r;
  }
  get diameter() {
    return this.r * 2;
  }
  set diameter(d) {
    this.r = d / 2;
  }
}
const c = new Circle(4);
console.log(c.diameter);
c.diameter = 20;
console.log(c.r);
console.log(Object.keys(c).join(","));
