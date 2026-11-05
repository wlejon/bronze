// Compares whose only use is a branch, on the values where the fused
// compare-and-branch and the materialized result could disagree: -0 against
// 0, NaN against itself and anything, 0n against 0 and false, and the
// negated forms (`!(a === b)`, `(a == b) === false`) of each.
const vals = [0, -0, NaN, 0n, 1n, -1, 1, 0.5, Infinity, -Infinity, "", "0", false, true, null, undefined, {}];
function se(a, b) { if (a === b) return 1; return 0; }
function sne(a, b) { if (a !== b) return 1; return 0; }
function le(a, b) { if (a == b) return 1; return 0; }
function lne(a, b) { if (a != b) return 1; return 0; }
function nse(a, b) { if (!(a === b)) return 1; return 0; }
function nle(a, b) { if ((a == b) === false) return 1; return 0; }
function lt(a, b) { if (a < b) return 1; return 0; }
function nlt(a, b) { if (!(a < b)) return 1; return 0; }
function ge(a, b) { if (a >= b) return 1; return 0; }
function nge(a, b) { if (!(a >= b)) return 1; return 0; }
const fns = [se, sne, le, lne, nse, nle, lt, nlt, ge, nge];
const rows = [];
for (const a of vals) {
  let row = "";
  for (const b of vals) {
    let cell = 0;
    for (const f of fns) cell = cell * 2 + f(a, b);
    row += cell.toString(36).padStart(2, "0");
  }
  rows.push(row);
}
console.log(rows.join("\n"));

// Typed doubles: the compiler knows both sides are Numbers, so the branch is
// the float compare itself (ucomisd: NaN is unordered).
function fcmp(x, y) {
  let s = 0;
  if (x === y) s |= 1;
  if (x !== y) s |= 2;
  if (x == y) s |= 4;
  if (!(x == y)) s |= 8;
  if (x < y) s |= 16;
  if (!(x < y)) s |= 32;
  if (x <= y) s |= 64;
  if (!(x > y)) s |= 128;
  return s;
}
const ds = [0, -0, NaN, 1.5, -1.5, Infinity, -Infinity, 5e-324];
let acc = "";
for (let r = 0; r < 200; r++) {
  acc = "";
  for (const x of ds) for (const y of ds) acc += fcmp(x * 1, y * 1).toString(16).padStart(2, "0");
}
console.log(acc);

// A loop whose exit is a negated compare on a double that becomes NaN.
let x = 4, n = 0;
while (!(x < 0.5)) { x = x > 1 ? x / 2 : NaN; n++; if (n > 10) break; }
console.log(n, x);

// BigInt against Number and Boolean in loose equality branches.
const mixed = [[0n, 0], [0n, -0], [0n, false], [1n, true], [1n, 1.0], [2n ** 64n, 2 ** 64], [0n, NaN], [-1n, -1]];
console.log(mixed.map(([a, b]) => (a == b ? "T" : "F") + (a != b ? "t" : "f") + (a < b ? "<" : "") + (a > b ? ">" : "")).join(" "));
