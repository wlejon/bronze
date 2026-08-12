// `let` and `const` written directly in a switch case.
//
// ECMA-262 14.12.2 makes the whole CaseBlock ONE declarative environment, so
// `total` below is in scope in every clause — including the ones above the
// clause that initializes it. That is what makes a switch body the one scope
// with more entry points than it has declarations, and what makes it the shape
// the temporal dead zone exists for: entering `case 1` and reading `total` is
// 9.1.1.1.6's ReferenceError, decided by the binding's state at that moment
// rather than by where the read sits relative to the declaration.
//
// The second switch is the same declaration given a block of its own, which is
// an ordinary scope with one entry point and needs no dead zone at all.
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
