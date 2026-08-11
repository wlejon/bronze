// Arrow functions. The token exists (TokenKind::Arrow) but nothing parses
// it, so `x => x` is a parse error naming '=>' today. The last two cases
// are the reason arrows are not just shorter syntax: `this` is lexical, so
// an arrow inside a constructor sees the instance, and one at the top level
// does not shadow anything.
const double = (x) => x * 2;
console.log(double(4));
const add = (a, b) => { return a + b; };
console.log(add(1, 2));
const noArgs = () => 7;
console.log(noArgs());
const single = x => x + 1;
console.log(single(1));
console.log([1, 2, 3].map((x) => x * x).join(","));
const outer = 5;
const closes = () => outer * 2;
console.log(closes());
function Counter() {
  this.count = 10;
  this.get = () => this.count;
}
const c = new Counter();
console.log(c.get());
