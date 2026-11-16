// A getter read speculated, then the getter replaced, then turned into a
// data property on the prototype, then shadowed on an instance.
function main() {
  class G { constructor(x) { this._x = x; } get x() { return this._x; } }
  const gs = [new G(1), new G(2), new G(3)];
  let s = 0;
  for (let i = 0; i < 40000; i++) {
    if (i === 10000) Object.defineProperty(G.prototype, "x", { get() { return this._x * 10; }, configurable: true });
    if (i === 20000) Object.defineProperty(G.prototype, "x", { value: 7, writable: true, configurable: true });
    if (i === 30000) gs[1].x = 100;
    s += gs[i % 3].x;
  }
  console.log(s, gs.map(g => g.x).join(","));
}
main();
