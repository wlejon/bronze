// `finally` with `return` and `break` when the abrupt completion comes out of
// a CALL rather than a `throw` statement in the protected block: the throw
// unwinds out of the callee's frame and lands in this function's handler, and
// the finally's own completion then decides what leaves (ECMA-262 14.15.3:
// an abrupt F replaces B; a normal F lets B through).

function fail(tag) {
  throw new Error(tag);
}

function pass(v) {
  return v;
}

// The callee's throw is discarded by the finally's return.
function returnSwallowsCalleeThrow() {
  try {
    fail("a");
  } finally {
    return "finally-return";
  }
}
console.log(returnSwallowsCalleeThrow());

// A catch that rethrows via a call, then a finally that returns: the second
// throw is discarded too.
function returnSwallowsCatchThrow() {
  try {
    fail("b");
  } catch (e) {
    fail("from-catch:" + e.message);
  } finally {
    return "swallowed";
  }
}
console.log(returnSwallowsCatchThrow());

// `break` in a finally ends the loop and drops the callee's throw.
function breakSwallowsThrow() {
  var out = [];
  for (var i = 0; i < 5; i++) {
    try {
      out.push(pass(i));
      if (i === 2) fail("c");
    } finally {
      if (i === 2) break;
    }
  }
  return out.join(",");
}
console.log(breakSwallowsThrow());

// `continue` in a finally drops the throw and keeps iterating.
function continueSwallowsThrow() {
  var out = [];
  for (var i = 0; i < 4; i++) {
    try {
      if (i % 2 === 1) fail("odd");
      out.push("even" + i);
    } finally {
      continue;
    }
  }
  return out.join(",");
}
console.log(continueSwallowsThrow());

// A normal finally lets the callee's throw through to the caller, after the
// finally's side effects.
function normalFinallyRethrows() {
  var log = [];
  try {
    try {
      fail("d");
    } finally {
      log.push("inner-finally");
    }
  } catch (e) {
    log.push("caught:" + e.message);
  }
  return log.join(",");
}
console.log(normalFinallyRethrows());

// A return in the try whose finally calls something that throws: the throw
// replaces the pending return.
function finallyThrowReplacesReturn() {
  try {
    return "never";
  } finally {
    fail("e");
  }
}
try {
  finallyThrowReplacesReturn();
} catch (e) {
  console.log("replaced:" + e.message);
}

// The return value computed in the try survives a finally that calls a
// function which throws and catches internally.
function finallyWithInternalCatch() {
  var x = 10;
  try {
    return x;
  } finally {
    try {
      fail("f");
    } catch (e) {
      x = 99;
    }
  }
}
console.log(finallyWithInternalCatch());
