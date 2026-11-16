// A speculated add, then the prototype gains a setter for the key, then a
// read-only property for another key: the assignment must call the setter
// and (sloppy) silently not write.
function main() {
  class P { }
  let calls = 0;
  function f(o, i) { o.v = i; o.w = i + 1; return o; }
  let s = 0;
  for (let i = 0; i < 30000; i++) {
    if (i === 10000) Object.defineProperty(P.prototype, "v", { set(x) { calls++; this._v = x; }, get() { return this._v; }, configurable: true });
    if (i === 20000) Object.defineProperty(P.prototype, "w", { value: -5, writable: false, configurable: true });
    const o = f(new P(), i);
    s += o.v + o.w;
  }
  const last = f(new P(), 1);
  console.log(s, calls, Object.keys(last).join(","), last.w, Object.getOwnPropertyNames(last).join(","));
}
main();
