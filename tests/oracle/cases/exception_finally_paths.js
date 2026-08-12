// `finally` runs on EVERY path out of the protected region, and a completion
// from inside it replaces the one that was leaving.
//
// Every expectation below is read off ECMA-262 14.15.3 (Runtime Semantics:
// Evaluation of `try Block Finally` / `try Block Catch Finally`): B is
// evaluated, then F; "if F.[[Type]] is normal, set F to B" — so the finally's
// completion wins exactly when it is abrupt, and the try's wins otherwise.
// 14.15.1 gives the two-part form, and 14.15.2 the catch clause.

// The ordinary case: the finally's effects happen, its normal completion is
// discarded, and the try's return value is what reaches the caller.
function returnThroughFinally() {
  try {
    return "try";
  } finally {
    console.log("fin1");
  }
}
console.log(returnThroughFinally());

// 14.15.3 step 4 with F abrupt: the finally's `return` replaces the try's.
function returnOverridesReturn() {
  try {
    return 1;
  } finally {
    return 2;
  }
}
console.log(returnOverridesReturn());

// The same rule with B a throw completion: the pending exception is
// DISCARDED, not rethrown after the finally.
function returnOverridesThrow() {
  try {
    throw "boom";
  } finally {
    return 3;
  }
}
console.log(returnOverridesThrow());

// A `break` is an abrupt completion of B like any other (6.2.4), so the
// finally runs before control leaves the loop.
function breakThroughFinally() {
  var out = [];
  for (var i = 0; i < 3; i++) {
    try {
      if (i === 1) break;
      out.push("body" + i);
    } finally {
      out.push("fin" + i);
    }
  }
  return out.join(",");
}
console.log(breakThroughFinally());

// And so is `continue`: the finally runs once per iteration, on the
// continuing iterations as well as the falling-through ones.
function continueThroughFinally() {
  var out = [];
  for (var i = 0; i < 3; i++) {
    try {
      if (i === 1) continue;
      out.push("b" + i);
    } finally {
      out.push("f" + i);
    }
  }
  return out.join(",");
}
console.log(continueThroughFinally());

// A labelled break crossing two finallys runs BOTH, innermost first: the
// inner try's completion is the abrupt one that the outer try's B produces,
// so the outer finally sees it in turn.
function labelledBreakCrossesTwo() {
  var out = [];
  outer: for (var i = 0; i < 2; i++) {
    try {
      try {
        out.push("in");
        break outer;
      } finally {
        out.push("inner-fin");
      }
    } finally {
      out.push("outer-fin");
    }
  }
  out.push("after");
  return out.join(",");
}
console.log(labelledBreakCrossesTwo());

// A throw raised INSIDE a finally replaces the one on its way out, and it is
// the enclosing handler that catches it — not this statement's own, which
// would run the finally a second time.
function throwFromFinally() {
  try {
    try {
      throw "first";
    } finally {
      throw "second";
    }
  } catch (e) {
    return e;
  }
}
console.log(throwFromFinally());

// A `catch` is protected by its own statement's `finally` (14.15.3 evaluates
// B as the whole `try Block Catch` when all three parts are written).
function finallyCoversCatch() {
  var out = [];
  try {
    try {
      throw "x";
    } catch (e) {
      out.push("caught:" + e);
      throw "again";
    } finally {
      out.push("fin");
    }
  } catch (e2) {
    out.push("outer:" + e2);
  }
  return out.join(",");
}
console.log(finallyCoversCatch());

// Nothing pending, nothing thrown: a finally on a plain fall-through path.
var order = [];
try {
  order.push("t");
} finally {
  order.push("f");
}
order.push("after");
console.log(order.join(","));
