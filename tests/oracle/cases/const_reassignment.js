// Assignment to a `const` binding, which ECMA-262 makes a TypeError in SLOPPY
// code as well as in strict: 14.3.1.1 creates the binding with
// CreateImmutableBinding(name, true), and the `true` is the S that 9.1.1.1.5
// SetMutableBinding step 4 tests. The strictness of the code doing the
// assigning never enters into it.
//
// bronze answers both shapes below by STORING, which is neither arm of that
// step — sloppy code that is not supposed to throw is still not supposed to
// write. Two separate reasons, found while measuring stage E2:
//
//   - `lower_scope.cpp emitEnvSet` gates the immutable arm on `strictCode_`,
//     so a script (which is sloppy unless it says otherwise) drops through to
//     the ordinary store instead of dropping the store;
//   - a `const` that nothing captures never becomes an environment slot at
//     all, so it never reaches that code: it is an SSA value and the
//     assignment is a rename.
//
// Both are pre-existing and neither is about the environment access path.
function direct() {
  const a = 1;
  try { a = 2; } catch (e) { return `${e.name}/${a}`; }
  return `no-throw/${a}`;
}

function viaClosure() {
  const b = 1;
  const write = () => { b = 2; };
  try { write(); } catch (e) { return `${e.name}/${b}`; }
  return `no-throw/${b}`;
}

function compound() {
  const c = 1;
  const bump = () => { c += 1; };
  try { bump(); } catch (e) { return `${e.name}/${c}`; }
  return `no-throw/${c}`;
}

function destructured() {
  const d = 1;
  const write = () => { ({ d } = { d: 5 }); };
  try { write(); } catch (e) { return `${e.name}/${d}`; }
  return `no-throw/${d}`;
}

function forOfBinding() {
  for (const e of [1]) {
    try { e = 2; } catch (err) { return `${err.name}/${e}`; }
    return `no-throw/${e}`;
  }
  return "unreached";
}

console.log(direct(), viaClosure(), compound(), destructured(), forOfBinding());

// And in an explicitly strict function — the arm `emitEnvSet` does have code
// for. It stores too, so whatever `strictCode_` is when a nested arrow's
// assignment is lowered, it is not the enclosing function's `"use strict"`.
// Pinned in the same file so the two halves cannot drift apart.
function strictHalf() {
  "use strict";
  const f = 1;
  const write = () => { f = 2; };
  try { write(); } catch (e) { return `${e.name}/${f}`; }
  return `no-throw/${f}`;
}
console.log(strictHalf());
