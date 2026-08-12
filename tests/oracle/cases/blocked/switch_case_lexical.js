// BLOCKED: `let` and `const` directly in a switch case are a named hard error.
// The diagnostic is
//
//   unsupported construct: a 'let' declaration directly in a switch case (the
//   switch body is one scope, so a case jump could reach it uninitialized);
//   wrap the case body in a block
//
// so this case fails to BUILD rather than producing a wrong answer.
//
// The blocked `try`/`catch`/`throw` case carried the mechanism that would lift
// this. It carried half of it. ECMA-262 14.12.2 makes the whole CaseBlock one
// declarative environment, so `total` below is in scope in every clause and in
// its temporal dead zone in the clauses above its declaration; entering `case
// 1` and reading it is 9.1.1.1.6's ReferenceError. `throw` now exists to raise
// one — what does not exist is the uninitialized binding state that decides
// WHEN to raise it, which is the same gap `temporal_dead_zone.js` names. That
// case is the prerequisite for this one.
switch (1) {
  case 1:
    try {
      console.log(total);
    } catch (e) {
      console.log(e.name);
    }
    break;
  case 2:
    let total = 10;
    console.log(total);
    break;
}

// The fix the diagnostic suggests, which compiles today: a block of its own
// gives the declaration a scope that no case jump can enter past.
switch (2) {
  case 2: {
    let scoped = 20;
    console.log(scoped);
    break;
  }
}
