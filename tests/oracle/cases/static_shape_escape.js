// What a proven-layout instance owes once it stops being one.
//
// A layout is a claim about SLOTS. Nothing a program can observe is about
// slots, so every line below has to answer exactly what it answered before the
// claim existed: an added property, a delete, enumeration order, JSON, and an
// accessor installed on the prototype after instances already exist.

class P {
  constructor(a, b) {
    this.a = a;
    this.b = b;
  }
  sum() {
    return this.a + this.b;
  }
}

// A dynamic write with a key this compilation cannot name.
function poke(o, k, v) {
  o[k] = v;
  return o;
}

const p = new P(1, 2);
console.log(p.sum());

// Warm the sites FIRST, then break the shape: the cell is published against the
// two-field shape and has to simply stop matching.
let warm = 0;
for (let i = 0; i < 500; i++) warm += p.sum();
console.log(warm);

poke(p, "c", 10);
console.log(p.a + "," + p.b + "," + p.c);
console.log(p.sum());
console.log(Object.keys(p).join(","));
console.log(JSON.stringify(p));

let order = "";
for (const k in p) order += k;
console.log(order);

delete p.b;
console.log(Object.keys(p).join(","));
console.log(String(p.b));
console.log(p.a);

// Re-adding a deleted name puts it LAST (10.1.5 / 6.1.7.1), not back where it
// was — which is the exact thing a fixed layout must not "helpfully" restore.
p.b = 7;
console.log(Object.keys(p).join(","));
console.log(p.sum());

// A fresh instance is untouched by any of it.
const q = new P(4, 5);
console.log(q.sum());
console.log(Object.keys(q).join(","));

// An accessor installed on the prototype AFTER instances exist. An own property
// still shadows it; an object without one sees the getter.
Object.defineProperty(P.prototype, "b", {
  get() {
    return -1;
  },
  configurable: true,
});
console.log(q.b);
const r = Object.create(P.prototype);
console.log(r.b);
console.log(q.sum());
