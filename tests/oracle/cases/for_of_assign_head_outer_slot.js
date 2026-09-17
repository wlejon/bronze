// A `for (name of iterable)` head whose target is a bare identifier is an
// ASSIGNMENT to an existing binding (ECMA-262 14.7.5.7, ForIn/OfBodyEvaluation
// with lhsKind assignment), not a declaration. When that binding is an outer
// scope's environment slot — a module-level `let` the enclosing function
// captured — the head must resolve it the way `name = value` would: through the
// record, by depth and index. The capture walk once recorded no mention for a
// bare head, so the module-level `let` got no slot and the head inside a
// FUNCTION resolved the name as a global and threw, while the same head at the
// top level, and a head over the function's own local, both worked. The values
// pinned here are what the assignment form leaves in the slot after the loop.

let last = "none";
for (last of ["x", "y"]) {}
console.log("top level:", last);

function local() {
  let l = 0;
  for (l of [1, 2]) {}
  return l;
}
console.log("own local:", local());

function outer() {
  for (last of ["p", "q"]) {}
}
outer();
console.log("outer slot:", last);

let key = "";
function keys(obj) {
  for (key in obj) {}
}
keys({ a: 1, b: 2 });
console.log("for-in outer slot:", key);
