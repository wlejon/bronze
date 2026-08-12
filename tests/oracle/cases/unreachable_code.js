// Statements after a `return`, `break` or `continue` in the same statement
// list are unreachable, and unreachable code has no effect. That is the
// language's answer and not a convenience: a JS engine never runs it either.
//
// bronze did not merely run it, it failed to COMPILE it — the unreachable
// statements were lowered into a block that already held its terminator, and
// the IL verifier rejected the result, so `function f() { return 3; g(); }`
// reported an internal message and produced no executable at all.

function early() {
  return "first";
  console.log("never");
}
console.log(early());

// What a dead region still contributes is hoisting: a function declared below
// the `return` is bound before any statement runs, so the call above it
// resolves. Skipping the dead statements must not skip that, which is why the
// two are separate passes.
function hoistsFromDeadRegion() {
  return helper();
  function helper() { return "hoisted"; }
}
console.log(hoistsFromDeadRegion());

// `break` and `continue` end their statement lists the same way a `return`
// does — they are terminators of the block they sit in, not only of loops.
let seen = "";
for (let i = 0; i < 4; i++) {
  if (i === 1) {
    continue;
    seen += "skipped";
  }
  if (i === 3) {
    break;
    seen += "skipped";
  }
  seen += i;
}
console.log(seen);

// A function that returns no value still EVALUATES to one: undefined,
// whether it says so with a bare `return` or falls off the end. bronze
// compiles both to a void IL function, and the undefined has to be
// materialized at the call site — reading the void call's absent result
// instead was a compile failure wherever such a call was used as a value.
function bare() { return; }
function fallsOff() { }
console.log(bare());
console.log(fallsOff());
console.log(bare() === undefined);
console.log("v: " + fallsOff());
