// In a .js file every token sequence means what JavaScript says, even where a
// TypeScript file would read the same tokens differently. bronze reads
// TypeScript natively, and these are the sequences its TypeScript grammar
// reads as type arguments: in TypeScript `a<b>(c)` is a generic call, and
// `f(lo < hi, v > (0))` a call of `lo<hi, v>(0)`. In JavaScript both are
// relational operators (ECMA-262 13.10), left-associative, so `a < b > (c)`
// is `(a < b) > c`, a boolean compared with a number.
const a = 1, b = 2, c = 3;
console.log(a < b > (c));
console.log(a < b > c);
console.log(a<b>(c));

function sum(p, q) { return p + "|" + q; }
const lo = 1, hi = 2, v = 3;
console.log(sum(lo < hi, v > (0)));
console.log([lo < hi, v > (0)]);

const o = { n: 1, m() { return 5; } };
console.log(o.n < 2 > (1));
console.log(o.m() < o.n > (0));

// `new` and an optional chain are the other places TypeScript takes type
// arguments; JavaScript has none there either.
class K { constructor(x) { this.x = x; } }
const lt = K < K, gt = K > (K);
console.log(lt, gt);
const maybe = { f: 2 };
console.log(maybe?.f < 3 > (0));

// `async` is an ordinary identifier unless an arrow follows it.
const async = 4;
console.log(async < b > (c));

// The prefix spelling of a TypeScript type assertion, `<T>x`, is not an
// expression in JavaScript at all; what follows is the comparison chain the
// same characters make when the left operand is written.
const T = 0;
console.log(T < a > (0));
