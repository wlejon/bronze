// console.log of the non-numeric primitives. `null` is why this case
// exists: it carries its own NaN-box tag (0xFFF5), so it never satisfied
// the undefined check in bronze_print_value and fell through to the object
// branch, printing `[object]` where node prints `null`. Nothing caught it —
// before this case `null` appeared in the suite only as an operand
// (`null ?? 5` in short_circuit), never as a printed value. Number printing
// is pinned by number_formatting and is deliberately not re-derived here.
console.log(null);
console.log(undefined);
console.log(true);
console.log(false);
console.log("");
console.log("text");

// null through every path that stores and re-reads it, not just a literal
// handed straight to the call: a binding, a branch join, a property slot,
// an array element, and a function return.
let x = null;
console.log(x);
console.log(true ? null : 1);
const o = { p: null };
console.log(o.p);
const a = [null];
console.log(a[0]);
function f() {
  return null;
}
console.log(f());

// null and undefined stay distinct everywhere they are observable.
console.log(null === undefined);
console.log(null === null);
console.log(null ?? "from null");
console.log(undefined ?? "from undefined");

// A property that does not exist reads undefined, not null.
console.log(o.absent);
