// The corners of Array.prototype that a naive implementation gets wrong:
// negative relative indices, the empty array, SameValueZero vs === (NaN),
// join's one exception to ToString, the callback's index/array arguments, and
// the mutators that move elements rather than append them. Every expectation is
// ECMA-262, derived by hand.
const a = [1, 2, 3, 4, 5];
console.log(a.slice(-2).join(","));
console.log(a.slice(1, -1).join(","));
console.log(a.at(-1));
console.log(a.at(10));
console.log([].join("-"));
console.log([null, undefined, 1].join("|"));
console.log([NaN].includes(NaN));
console.log([NaN].indexOf(NaN));
console.log([1, [2, 3]].concat(4, [5, 6]).length);
console.log([1, 2, 3].reduceRight(function (acc, x) { return acc + "" + x; }, ""));
console.log([1, 2, 3].findLast(function (x) { return x < 3; }));
console.log([1, 2, 3].map(function (x, i, arr) { return x * i + arr.length; }).join(","));
const s = [3, 1, 2];
s.unshift(0, -1);
console.log(s.join(","));
console.log(s.shift());
console.log(s.join(","));
console.log([1, 2, 3].fill(9, 1).join(","));
console.log([1, 2, 3].every(function (x) { return x > 0; }));
console.log([1, 2, 3].some(function (x) { return x > 2; }));
let sum = 0;
[1, 2, 3].forEach(function (x) { sum = sum + x; });
console.log(sum);
