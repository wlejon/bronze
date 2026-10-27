// A BigInt made by arithmetic (2n ** k) is a heap cell: the inline element,
// property and captured-variable stores of one take the write barrier, which
// the gc-stress runs of this case check (every allocation collects).
const a = [0, 0]; const o = { v: 0 };
const junk = [];
for (let i = 0; i < 2000; i++) junk.push({ i });
function put(k) { a[0] = 2n ** BigInt(k); o.v = 3n ** BigInt(k); }
for (let k = 60; k < 70; k++) { put(k); for (let j = 0; j < 200; j++) junk[j] = { j }; }
console.log(a[0], o.v);
const cls = [];
for (let i = 0; i < 50; i++) { const v = 2n ** BigInt(i); cls.push(() => v); }
console.log(cls[0](), cls[10](), cls[49]());
