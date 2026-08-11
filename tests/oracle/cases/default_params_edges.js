// The three rules about a default value that are easy to get wrong, each
// pinned by a line that would print differently under the wrong one
// (docs/0017 decision 1).
//
// 1. `undefined` and NOTHING ELSE fires a default. ECMA-262 10.2.11 binds a
//    formal to its default only when the argument is undefined, so `null`,
//    `0` and `""` all bind through — a truthiness test here would print
//    "1:B" three times instead of once.
// 2. A default is evaluated ON EACH CALL that omits the argument, in the
//    function's own scope. So `eff` leaves `n` at the number of calls that
//    omitted `v` (two of three), `useBase` sees the CURRENT `base` rather
//    than the one at definition, and `fresh` gets a new array every time
//    rather than one shared between calls.
// 3. Defaults are evaluated left to right and each sees the parameters
//    before it, so `order(1)` fills in 2 and then 3 from it.
function f(a, b = "B") { return a + ":" + b; }
console.log(f(1));
console.log(f(1, undefined));
console.log(f(1, null));
console.log(f(1, 0));
console.log(f(1, ""));
let n = 0;
function eff(v = (n = n + 1)) { return v; }
eff();
eff(50);
eff();
console.log(n);
function order(a, b = a + 1, c = b + 1) { return a + "," + b + "," + c; }
console.log(order(1));
console.log(order(1, 10));
let base = 10;
function useBase(v = base) { return v; }
console.log(useBase());
base = 20;
console.log(useBase());
function fresh(a = []) { a.push(1); return a.length; }
console.log(fresh());
console.log(fresh());
class C { scale(v = 2) { return v * 3; } }
console.log(new C().scale());
console.log(new C().scale(4));
const arrow = (x, y = x * 2) => x + y;
console.log(arrow(3));
console.log(arrow(3, 1));
