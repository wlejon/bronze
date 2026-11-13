// A function whose guard fails on every call: each receiver has a shape of
// its own, so the speculated code leaves for the resume twin each time and
// the program's answers are the generic code's.

function read(o) {
  return o.p + o.q;
}

function make(i) {
  const o = {};
  o['k' + i] = i;                // a different shape for every i
  o.p = i;
  o.q = 1;
  return o;
}

let s = 0;
const objs = [];
for (let i = 0; i < 400; i++) objs.push(make(i));
for (let r = 0; r < 60; r++) {
  for (let i = 0; i < objs.length; i++) s += read(objs[i]);
}
console.log(s);

class C { constructor() { this.p = 1; this.q = 2; } }
const c = new C();
let t = 0;
for (let i = 0; i < 30000; i++) t += read(i & 1 ? c : objs[i % 400]);
console.log(t);
console.log(read(null === undefined ? null : { p: 'a', q: 'b' }), read({ q: 1 }));
try { read(null); } catch (e) { console.log(e instanceof TypeError); }
