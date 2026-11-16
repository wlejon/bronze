// Two classes with one name, so a harvest's shape names cannot tell their
// instances apart: the speculated read's shape cell takes the first class's
// shape, and the hot loop reads the second class's objects, failing the
// guard on every call. After enough exits the speculated function retires
// and its calls go to the generic one; every call keeps its right answer.
const A = (() => class Foo { constructor(x) { this.x = x; this.y = 1; } })();
const B = (() => class Foo { constructor(x) { this.x = x; this.y = 2; } })();
function getXY(o) {
  return o.x * 2 + o.y;
}
function main() {
  const a = new A(5);
  let s = getXY(a);
  const bs = [];
  for (let i = 0; i < 16; i++) bs.push(new B(i));
  for (let r = 0; r < 40000; r++) {
    s += getXY(bs[r & 15]);
    if ((r & 1023) === 0) s += getXY(a);
  }
  console.log(s);
}
main();
