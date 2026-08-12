// Spread in calls, array literals and object literals.
//
// What each line pins: a spread call passes the elements as arguments, to a
// builtin and to a declared function alike; a spread inside an array literal
// splices rather than nests; an object spread copies own enumerable
// properties and a later key wins; a spread copy is a FRESH array, so
// pushing to it leaves the original's length alone; spreading a string walks
// it by code point (the same walk for-of does); and a
// constructor's rest parameter receives every argument `new` was given.
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
