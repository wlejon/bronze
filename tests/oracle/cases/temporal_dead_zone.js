// The temporal dead zone: a `let` or `const` binding exists from the moment
// its scope is entered and holds no value until its declaration is evaluated,
// and a read in between is a ReferenceError.
//
// ECMA-262 9.1.1.1.6 GetBindingValue: "if the binding for N in envRec is an
// uninitialized binding, throw a ReferenceError". 14.3.1 CreateMutableBinding
// for a `let` creates the binding uninitialized, so every read below that runs
// before the initializer does is a ReferenceError — rather than `undefined`,
// and rather than the enclosing scope's binding of the same name.
//
// Three shapes, and the third is the one that says what "temporal" means:
//
//   - an inner `let x` shadows the outer `x` from the top of the BLOCK, not
//     from the line it is written on, so the read above it does not answer 1;
//   - a read above a declaration in a function body is the same rule with the
//     function body as the scope;
//   - `early` is written above `let later` and reads it from inside a call.
//     Called before the declaration runs it throws; called after it, it
//     answers. One source position, two answers, because the dead zone is a
//     property of a moment in evaluation and not of a position in the text.
//
// Edge shapes around the same mechanism — `typeof`, a class, a per-iteration
// loop binding, an assignment — are in temporal_dead_zone_edges.js.
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
