// BLOCKED: an accessor property is `unsupported construct: object literal
// getter or setter (accessor properties are not implemented)` in the parser
// and `unsupported construct: class getter or setter` in lowering.
//
// The blocker is that a property in bronze is a VALUE in a slot. docs/0004
// gives an object a shape and a flat slot vector, and every read is
// `shape.lookupProperty` then a load — there is no place to record that a
// slot holds a pair of functions to be CALLED instead, and no place for the
// inline caches of docs/0008 to record that a hit must become a call. The
// shape node is where the answer belongs (it already carries `enumerable`
// for docs/0018 decision 2), so the work is a property-attribute kind on the
// shape plus an IC state that means "accessor, do not fold to a slot load" —
// after which `Object.defineProperty` is mostly the same machinery.
//
// What this case pins when it lands, from ECMA-262 6.1.7.1 (Property
// Attributes), 10.5.1 and 15.7.14 (class accessors):
//
// 1. A getter runs on READ, at the site of the read, with `this` bound to the
//    receiver — so `full` recomputes after `first` changes rather than
//    holding a value captured at definition.
// 2. A setter runs on WRITE and its return value is discarded; the property
//    afterwards is whatever the getter says, not what was assigned. Assigning
//    "Ada Lovelace" therefore shows up as two separate fields.
// 3. An accessor with only a getter silently ignores a write in sloppy mode
//    — `area` keeps computing from `side`.
// 4. An accessor defined in an object literal is ENUMERABLE, so it appears in
//    `Object.keys` and its getter runs when the key is read back. A class
//    accessor is not: 15.7.14 defines it with `enumerable: false`, the same
//    rule that already keeps class methods out of enumeration.
// 5. An accessor on a PROTOTYPE is found by the ordinary proto walk and runs
//    with `this` as the instance, not as the prototype — which is what makes
//    a getter usable as a computed field on every instance at once.
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
