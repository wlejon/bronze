// Dense arrays grow on contiguous append; a write past the end is a sparse
// write and stays a named hard error until dictionary elements land (0004).
const a = [1, 2, 3];
console.log(a.length);
a[3] = 4;
console.log(a.length);
console.log(a[3]);

const b = [];
console.log(b.length);
let i = 0;
while (i < 100) {
  b[i] = i * 2;
  i = i + 1;
}
console.log(b.length);
console.log(b[0]);
console.log(b[50]);
console.log(b[99]);
console.log(b[100]);

// Growth must preserve identity: `c` and `alias` are the same array.
const c = [7];
const alias = c;
let j = 1;
while (j < 40) {
  c[j] = j;
  j = j + 1;
}
console.log(alias.length);
console.log(alias[39]);
console.log(alias[0]);
