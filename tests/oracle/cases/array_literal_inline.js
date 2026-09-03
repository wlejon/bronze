// A small array literal is bump-allocated by generated code
// (llvm_construct.cpp) as the same two heap objects bronze_create_array
// makes: the header and a HOLE-filled elements block at the runtime's
// capacity floor. Everything an array does afterwards — grow, hold holes,
// take named properties, be destructured, be collected — has to be
// indifferent to which path built it.

function cellCenter(cx, cy) { return [cx * 2, cy * 3]; }
let acc = 0;
for (let i = 0; i < 20000; i++) {
  const [x, z] = cellCenter(i & 7, i >> 3);
  acc += x + z;
}
console.log(acc);

// Every literal size around the floor, and past what the inline path takes.
const lits = [[], [1], [1, 2], [1, 2, 3], [1, 2, 3, 4], [1, 2, 3, 4, 5], [1, 2, 3, 4, 5, 6, 7, 8], [1, 2, 3, 4, 5, 6, 7, 8, 9]];
console.log(lits.map(a => a.length + ':' + a.join('')).join(' '));

// Growth past the floor keeps the elements and the identity.
const g = [1, 2];
const same = g;
for (let i = 3; i <= 12; i++) g.push(i);
console.log(g.length, g.join(','), same === g, g[11], g[12]);

// Holes and elisions beside a literal's own elements.
const h = [1, , 3];
console.log(h.length, 1 in h, h[1], h.indexOf(undefined), JSON.stringify(h));
const e = [];
e[5] = 'x';
console.log(e.length, JSON.stringify(e));

// Named properties on a literal, and its methods.
const n = [3, 1, 2];
n.tag = 'sorted';
console.log(n.sort().join(''), n.tag, Object.keys(n).join(','), n.map(v => v * 2).join(','));

// Nested literals, spread and rest.
const nest = [[1, [2, 3]], [4]];
const [[a, [b, c]], [d]] = nest;
console.log(a + b + c + d, [...nest[0], ...nest[1]].length, JSON.stringify(nest));
function rest(...r) { return r.length + ':' + r.join(''); }
console.log(rest(), rest(1), rest(1, 2, 3, 4, 5));

// Enough churn to cross the allocation window many times, keeping every
// array alive so a collection has to forward the inline-built ones.
const keep = [];
for (let i = 0; i < 50000; i++) {
  const t = [i, i + 1, i + 2];
  if (i % 997 === 0) keep.push(t);
  keep[0] = keep[0] || t;
}
let sum = 0;
for (const t of keep) sum += t[0] + t[1] + t[2];
console.log(keep.length, sum);

// A literal handed to a builtin that reads it as an iterable.
console.log(new Set([1, 1, 2]).size, new Map([[1, 'a'], [2, 'b']]).get(2), Math.max(...[3, 9, 4]));
