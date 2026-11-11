// A name no Array has, read off arrays (three.js's `v.x` / `v.r` on an array
// uniform) — the answer must follow Object.prototype and the array itself.
function readX(v) { return v.x; }
function readR(v) { return v.r; }
const a = [1, 2, 3];
const vec = { x: 1 }, col = { r: 2 };
const seen = [];
for (let i = 0; i < 6; i++) seen.push(String(readX([vec, a, col][i % 3])), String(readR([col, a, vec][i % 3])));
console.log(seen.join(","));
Object.prototype.x = "fromProto";
console.log(readX(a), readX([]), String(readR(a)));
delete Object.prototype.x;
console.log(String(readX(a)));
let hits = 0;
readR(a); readR(a); readR([9]);
console.log(hits);
delete Object.prototype.r;
readR(a);
console.log(hits, String(readR(a)));
// Own named properties and a custom prototype on the array itself.
const b = [4, 5];
b.x = "own";
const c = [6];
Object.setPrototypeOf(c, { x: "custom", __proto__: Array.prototype });
console.log(readX(b), readX(c), String(readX(a)), c.length);
// Names Array.prototype does have stay methods.
const names = ["map", "push", "at", "toString"];
console.log(names.map((n) => typeof a[n]).join(","), typeof readX.call(null, a));
