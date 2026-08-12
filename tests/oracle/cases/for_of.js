// for-of over an array and over a string. break and continue have to reach the
// right blocks, and the loop variable is per-iteration — the closures captured
// in the last case each see their own `x`, not a shared one. Strings iterate by
// code point, which is why the walk has its own advance step rather than an
// index increment.
const a = [10, 20, 30];
let sum = 0;
for (const x of a) { sum = sum + x; }
console.log(sum);
let out = "";
for (const ch of "abc") { out = out + ch + "-"; }
console.log(out);
let firstTwo = "";
for (const x of a) { if (x === 30) { break; } firstTwo = firstTwo + x; }
console.log(firstTwo);
let skipped = 0;
for (const x of a) { if (x === 20) { continue; } skipped = skipped + x; }
console.log(skipped);
for (const x of []) { console.log("never"); }
let last = 0;
for (let x of a) { x = x + 1; last = x; }
console.log(last);
const fns = [];
for (const x of a) { fns.push(() => x); }
console.log(fns[0]() + fns[2]());
