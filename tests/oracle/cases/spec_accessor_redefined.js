// A getter and a setter on a class's prototype read and written in a hot
// loop. Partway one instance gets the key as its own data property, then the
// getter and the setter are redefined in place, so a harvest of the run names
// the redefined pair: the declared pair and the own property must take the
// generic path, the redefined one the speculated path.
class P {
  constructor() { this._x = 0; this.n = 0; }
  get x() { return this._x; }
  set x(v) { if (this._x !== v) { this._x = v; this.n++; } }
}
function loop(p, n) {
  let s = 0;
  for (let i = 0; i < n; i++) { p.x = i & 15; s += p.x; }
  return s;
}
function main() {
  const out = [];
  let p = new P();
  out.push(loop(p, 30000), p.n);
  const own = new P();
  Object.defineProperty(own, "x", { value: 9, writable: true });
  out.push(loop(own, 30000), own.n, Object.keys(own).join(","));
  Object.defineProperty(P.prototype, "x", {
    get() { return this._x * 10; },
    set(v) { this._x = v; this.n += 2; },
    configurable: true,
  });
  out.push(loop(p, 30000), p.n);
  p = new P();
  out.push(loop(p, 30000), p.n);
  console.log(out.join(" "));
}
main();
