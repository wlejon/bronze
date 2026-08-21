// One `new` site passing a string is enough to take the whole name back.
//
// The join over a constructor's call sites is a proof only because it is a
// join: a site that passes a String widens the parameter to Dynamic, the
// constructor's `this.v = v` stops being a Number write, and the audit refuses
// the NAME `v` program-wide. Everything downstream then devolves to the boxed
// path and the program still means what the language says it means.
//
// The granularity is the point. `v` is refused; `p`, written by a class whose
// every site passes a Number, is not — one class's dynamism does not cost the
// program its other names.

class Box {
  constructor(v) {
    this.v = v;
  }
}

class Pt {
  constructor(p = 0) {
    this.p = p;
  }
}

function unbox(b) {
  return b.v;
}

function twice(q) {
  return q.p + q.p;
}

const n = new Box(1);
const s = new Box("hi");
const t = new Box(true);
console.log("number=" + (unbox(n) + 1));
console.log("string=" + (unbox(s) + "!"));
console.log("bool=" + unbox(t) + " " + typeof unbox(t));

// A string written from outside, into a field whose class body only ever wrote
// a parameter. Nothing here may read it as a double.
s.v = "bye";
console.log("rewritten=" + s.v + " length=" + s.v.length);
n.v = 2.5;
console.log("renumbered=" + n.v + " " + (n.v * 4));

const c = new Pt(3);
const d = new Pt();
console.log("clean=" + twice(c) + "," + twice(d) + "," + c.p + "," + d.p);
c.p = 1.25;
console.log("clean mutated=" + twice(c));

// The two live side by side on the same heap, and the dynamic readers see
// exactly what the language says is there.
console.log("json=" + JSON.stringify(s) + JSON.stringify(c));
console.log("keys=" + Object.keys(n).join(",") + "/" + Object.keys(c).join(","));

let sum = 0;
for (let i = 0; i < 100; i++) {
  sum = sum + twice(new Pt(i)) + unbox(new Box(i)) * 0;
}
console.log("loop=" + sum);

let text = "";
for (let i = 0; i < 4; i++) {
  text = text + unbox(new Box("s" + i));
}
console.log("text=" + text);
