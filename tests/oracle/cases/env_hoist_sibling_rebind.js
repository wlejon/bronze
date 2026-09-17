// A nested function's read of an outer `let`/`var` slot may be hoisted to its
// entry only when nothing in the OWNING scope's reach rebinds the slot — and
// the reader's own body is the wrong place to decide that. Each block below
// has a reader that never writes the slot itself and calls a sibling that
// does; a reader that answered from its own statements hoisted the load above
// the call and printed the value from before it.
//
// The shapes: a module-level `let` initialized to null and filled lazily by
// a sibling (the flora-lab worker's bloom bases), the same inside a function
// scope, a `var`, an update operator, a destructuring assignment, a compound
// assignment, and — the control — a slot no sibling writes, which the hoist
// is still allowed for. (A for-of ASSIGNMENT head over an outer slot is the
// same shape and is pinned in blocked/for_of_assign_head_outer_slot.js.)

let lazy = null;
function fill() { if (lazy) return; lazy = { v: 1 }; }
function useLazy() { fill(); return lazy.v; }
console.log("module let:", useLazy(), useLazy());

let count = 0;
function bump() { count++; }
function useCount() { bump(); bump(); return count; }
console.log("update op:", useCount());

var v = "before";
function setV() { v = "after"; }
function useV() { const seen = [v]; setV(); seen.push(v); return seen.join(","); }
console.log("var:", useV());

let a = 0, b = 0;
function destructure() { [a, b] = [7, 8]; }
function useAB() { destructure(); return a + b; }
console.log("destructuring:", useAB());

let text = "a";
function append() { text += "b"; }
function useText() { append(); return text; }
console.log("compound assign:", useText());

function scope() {
  let inner = null;
  function fillInner() { inner = "filled"; }
  function readInner() { fillInner(); return inner; }
  return readInner();
}
console.log("function scope:", scope());

const fixed = { v: "const" };
let stable = "stable";
function readStable() { fill(); return fixed.v + "/" + stable; }
console.log("control:", readStable());
