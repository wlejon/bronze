// Destructuring, in declarations, parameters and assignments. Every form
// here is a parse error today. Results are printed as scalars or joined
// strings on purpose: console.log of a container still has no pinned format
// (docs/0009), and a blocked case must not decide that in an .expected.
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
