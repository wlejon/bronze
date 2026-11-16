// A method call speculated on a prototype method, then the method is
// replaced on one instance, then on the prototype.
function main() {
  class V { constructor(x) { this.x = x; } m() { return this.x; } }
  const vs = []; for (let i = 0; i < 8; i++) vs.push(new V(i));
  let s = 0;
  for (let i = 0; i < 40000; i++) {
    if (i === 15000) vs[3].m = function () { return 1000; };
    if (i === 25000) V.prototype.m = function () { return -this.x; };
    s += vs[i % 8].m();
  }
  console.log(s, vs[3].m(), vs[4].m());
}
main();
