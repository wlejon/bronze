// Spread, in calls, array literals and object literals. `...` is a parse
// error today wherever it appears. Spreading a string iterates it by code
// point, the same walk for-of does (docs/0012 decision 2), and a spread copy
// is a fresh array — mutating it must not touch the original.
const nums = [1, 2, 3];
console.log(Math.max(...nums));
const more = [0, ...nums, 4];
console.log(more.join(","));
function sum3(a, b, c) { return a + b + c; }
console.log(sum3(...nums));
const merged = { ...{ a: 1 }, b: 2 };
console.log(merged.a + merged.b);
const copy = [...nums];
copy.push(4);
console.log(nums.length + ":" + copy.length);
const chars = [..."abc"];
console.log(chars.join("-"));
class Wrapper { constructor(...items) { this.items = items; } }
console.log(new Wrapper(1, 2, 3).items.length);
