// BLOCKED: bronze has no temporal dead zone, and no `ReferenceError`.
//
// ECMA-262 9.1.1.1.6 GetBindingValue: "if the binding for N in envRec is an
// uninitialized binding, throw a ReferenceError". 14.3.1 CreateMutableBinding
// for a `let` leaves the binding uninitialized until its LexicalBinding is
// evaluated, so every read below is a ReferenceError rather than `undefined`.
//
// `throw` landing (docs/0020) supplied the mechanism and not the state. What
// is missing is a third answer for a binding — initialized, uninitialized,
// and the check on every read that distinguishes them. bronze has a Hole tag
// (docs/0004 decision 1, `0xFFF7`) but it is spoken for: docs/0020 uses it as
// the empty pending-exception cell, so the dead zone needs a marker of its
// own rather than a reuse of that one.
//
// Two things are wanted with it. `ReferenceError` is not one of the three
// constructors docs/0020 decision 7 builds, so it has to be added; and the
// check has a cost on every read of an env-backed `let`, which is a place
// inference should be able to prove the check away and today cannot.
//
// Until then bronze reads the uninitialized binding as `undefined`, which is
// a silent wrong answer — so this case exists to keep it named.
let x = 1;
{
  try {
    console.log(x);
  } catch (e) {
    console.log(e.name);
  }
  let x = 2;
  console.log(x);
}
console.log(x);

function readBeforeDeclaration() {
  try {
    return v;
  } catch (e) {
    return e.name;
  }
  let v = 1;
}
console.log(readBeforeDeclaration());

// The dead zone is TEMPORAL, not positional: `hoisted` is called before the
// declaration is evaluated even though the read is written after it.
function early() {
  return later;
}
try {
  console.log(early());
} catch (e) {
  console.log(e.name);
}
let later = "visible";
console.log(early());
