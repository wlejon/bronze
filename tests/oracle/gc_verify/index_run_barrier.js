// An index run storing young objects far into a large (old) array: the
// barrier must dirty the slot's card, not the block's first.
function fill(a, x, y, z) {
  a[3000] = x;
  a[3001] = y;
  a[4090] = z;
}
const big = new Array(4096);
for (let i = 0; i < 4096; i++) big[i] = i;
let junk = [];
for (let i = 0; i < 200000; i++) junk.push({ i });   // promote big
junk = null;
let sum = 0;
for (let r = 0; r < 200; r++) {
  fill(big, { v: r }, { v: r + 1 }, { v: r + 2 });
  let t = [];
  for (let i = 0; i < 20000; i++) t.push({ i });      // force minors
  sum += big[3000].v + big[3001].v + big[4090].v;
}
console.log(sum);
