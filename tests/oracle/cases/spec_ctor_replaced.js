// The constructor a `new` site sees is replaced after warm-up by another
// closure of the same code (another prototype, so another instance shape),
// then by one returning an object.
function maker(tag) {
  function F(v, r) { this.v = v; this.w = tag; if (r) return { v: -v, w: "ret" }; }
  F.prototype.k = function () { return tag; };
  return F;
}
function main() {
  let C = maker("one");
  function make(i, r) { return new C(i, r); }
  let s = 0; const seen = new Set();
  for (let i = 0; i < 40000; i++) {
    if (i === 30000) C = maker("two");
    const o = make(i, i === 39999);
    s += o.v; seen.add(o.w); if (o.k) seen.add(o.k());
  }
  console.log(s, [...seen].join(","));
}
main();
