// A method and a getter found two links up the chain; partway the class's
// prototype is re-parented onto another class's (Object.setPrototypeOf on
// the prototype) and the base constructor's own prototype link is swapped,
// so a harvest names the new chain's members: the old ones must take the
// generic path and the new ones the speculated path.
class Base { area() { return 1; } get kind() { return "base"; } }
class Alt { area() { return this.r * 10; } get kind() { return "alt"; } }
class Mid extends Base {}
class Leaf extends Mid { constructor(r) { super(); this.r = r; } }
function total(objs, n) {
  let s = 0, k = "";
  for (let i = 0; i < n; i++) {
    const o = objs[i & 3];
    s += o.area();
    if ((i & 1023) === 0) k += o.kind[0];
  }
  return s + ":" + k;
}
function main() {
  const objs = [new Leaf(1), new Leaf(2), new Leaf(3), new Leaf(4)];
  const out = [total(objs, 20000)];
  Object.setPrototypeOf(Mid.prototype, Alt.prototype);
  Object.setPrototypeOf(Mid, Alt);
  out.push(total(objs, 20000), total([new Leaf(5), new Leaf(6), objs[0], objs[1]], 20000));
  console.log(out.join(" "), objs[0] instanceof Base, objs[0] instanceof Alt);
}
main();
