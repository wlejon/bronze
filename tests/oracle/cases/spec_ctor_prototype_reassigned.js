// `new F` speculated, then F.prototype replaced partway: later instances
// get the new prototype, earlier ones keep the old.
function main() {
  function F(x) { this.x = x; this.y = x * 2; }
  F.prototype.sum = function () { return this.x + this.y; };
  const early = [], late = [];
  let s = 0;
  for (let i = 0; i < 40000; i++) {
    if (i === 20000) F.prototype = { sum() { return -1; }, tag: "new" };
    const o = new F(i);
    s += o.sum();
    if (i === 100) early.push(o);
    if (i === 30000) late.push(o);
  }
  console.log(s, early[0].sum(), late[0].sum(), late[0].tag, early[0].tag);
  console.log(Object.getPrototypeOf(early[0]) === Object.getPrototypeOf(late[0]), late[0] instanceof F);
}
main();
