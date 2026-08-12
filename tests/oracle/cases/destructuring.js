// Destructuring in declarations, parameters and assignments ( decisions 4 to
// 6).
//
// What each line pins: an array pattern reads by position and an object
// pattern by key; a rest element collects the tail as a fresh array; a
// default in a pattern fires on a missing property; `m: renamed` binds the
// property under a different name; patterns work in a parameter list as they
// do in a declaration; the swap `[s1, s2] = [s2, s1]` proves the right side
// is fully evaluated before any target is written; and patterns nest.
//
// Results are printed as scalars or joined strings on purpose: a container's
// inspect format is pinned by the printing cases, and mixing the two questions
// into one expectation would make a change to either move this file.
const [a, b] = [1, 2];
console.log(a + b);
const { x, y } = { x: 10, y: 20 };
console.log(x + y);
const [p, ...rest] = [1, 2, 3];
console.log(p + ":" + rest.length);
const { z = 5 } = {};
console.log(z);
const { m: renamed } = { m: 7 };
console.log(renamed);
function dist({ dx, dy }) { return dx + dy; }
console.log(dist({ dx: 1, dy: 2 }));
function head([first]) { return first; }
console.log(head([9, 8]));
let s1 = 1;
let s2 = 2;
[s1, s2] = [s2, s1];
console.log(s1 + "," + s2);
const [[i, j]] = [[4, 5]];
console.log(i * j);
