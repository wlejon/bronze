// A constructor parameter's DEFAULT is one of its call sites.
//
// `constructor(x = 0)` bound to a `new C()` binds the default expression's
// value, so the default's type joins into the parameter exactly as an
// argument's does — and a default that is not a Number takes the name down as
// surely as an argument that is not one would.
//
// The three shapes side by side: a Number default (certifies), a String
// default (refuses that name and no other), and a default this analysis does
// not read at all — an expression over an earlier parameter, which is code and
// not a literal, and which therefore contributes Dynamic.

class Tag {
  constructor(n = 0, label = "none", scaled = 1) {
    this.n = n;
    this.label = label;
    this.scaled = scaled;
  }
}

// `m`'s default reads `k`, which is a value and not a literal. The parameter
// devolves, and so does the field it is written into.
class Pair {
  constructor(k = 0, m = k + 1) {
    this.k = k;
    this.m = m;
  }
}

class Maybe {
  constructor(u = undefined, w = null, f = false) {
    this.u = u;
    this.w = w;
    this.f = f;
  }
}

function tagN(t) {
  return t.n;
}

function pairK(p) {
  return p.k;
}

const t1 = new Tag();
const t2 = new Tag(2.5, "two", 4);
console.log("tag1=" + t1.n + "," + t1.label + "," + t1.scaled);
console.log("tag2=" + t2.n + "," + t2.label + "," + t2.scaled);
console.log("tagN=" + (tagN(t1) + tagN(t2)));
console.log("label typeof=" + typeof t1.label + " " + t1.label.length);

// `undefined` for a defaulted parameter runs the default; `null` does not.
const t3 = new Tag(undefined, undefined, null);
console.log("tag3=" + t3.n + "," + t3.label + "," + t3.scaled);

const p1 = new Pair();
const p2 = new Pair(10);
const p3 = new Pair(10, 20);
console.log("pair=" + p1.k + "," + p1.m + " " + p2.k + "," + p2.m + " " + p3.k + "," + p3.m);
console.log("pairK=" + (pairK(p1) + pairK(p2) + pairK(p3)));

const m1 = new Maybe();
console.log("maybe=" + m1.u + "," + m1.w + "," + m1.f);
console.log("maybe typeof=" + typeof m1.u + "," + typeof m1.w + "," + typeof m1.f);
console.log("maybe json=" + JSON.stringify(m1));
console.log("maybe keys=" + Object.keys(m1).join(","));

t1.n = 7;
t1.label = "seven";
console.log("mutated=" + tagN(t1) + t1.label);

let sum = 0;
for (let i = 0; i < 60; i++) {
  sum = sum + tagN(new Tag(i)) + pairK(new Pair(i));
}
console.log("loop=" + sum);
