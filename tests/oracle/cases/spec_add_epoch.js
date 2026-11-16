// Speculated adds whose prototype chain changes partway in ways that do not
// touch the added keys (a method added, an accessor for another key, the
// chain's own prototype replaced): each retires the adds' stamps, and the
// adds must re-check and keep adding plain data properties.
class Base { }
class P extends Base { }
function fill(o, i) { o.a = i; o.b = i + 1; return o; }
function main() {
  let s = 0;
  for (let i = 0; i < 40000; i++) {
    if (i === 10000) P.prototype.extra = function () { return 1; };
    if (i === 20000) Object.defineProperty(Base.prototype, "c", { get() { return 5; }, configurable: true });
    if (i === 30000) Object.setPrototypeOf(Base.prototype, { a0: 1 });
    const o = fill(new P(), i);
    s += o.a + o.b + (o.c | 0) + (o.extra ? o.extra() : 0) + (o.a0 | 0);
  }
  const o = fill(new P(), 1);
  console.log(s, Object.keys(o).join(","), Object.getOwnPropertyDescriptor(o, "a").writable);
}
main();
