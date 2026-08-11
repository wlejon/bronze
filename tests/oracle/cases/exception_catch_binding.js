// The catch parameter is a BindingPattern, and it is optional.
//
// ECMA-262 14.15: `Catch : catch ( CatchParameter ) Block`, with
// CatchParameter being either a BindingIdentifier or a BindingPattern, and
// `Catch : catch Block` for the parameterless form (ES2019). 14.15.2
// CatchClauseEvaluation puts the parameter in a NEW declarative environment
// around the block, so it shadows rather than assigns.
//
// 14.14 ThrowStatement throws the value of its expression, whatever it is:
// there is no requirement that it be an Error.

// An object pattern destructures the thrown value.
try {
  throw { message: "m", code: 7 };
} catch ({ message, code }) {
  console.log(message, code);
}

// The parameter is block-scoped to the catch: the outer `e` is untouched.
var e = "outer";
try {
  throw "inner";
} catch (e) {
  console.log(e);
}
console.log(e);

// An array pattern, with a default and a rest element — a CatchParameter
// admits everything any other BindingPattern does (13.3.3).
try {
  throw [1, 2, 3];
} catch ([a, b = 99, ...rest]) {
  console.log(a, b, rest.join("|"));
}

// A default fires when the property is absent, exactly as in a declaration.
try {
  throw {};
} catch ({ code = 42 }) {
  console.log(code);
}

// The parameterless form: the value is unreachable, and that is the point.
try {
  throw 0;
} catch {
  console.log("bindingless");
}

// Any value is throwable, including the two nullish ones and a number.
try {
  throw undefined;
} catch (v) {
  console.log(typeof v);
}
try {
  throw null;
} catch (v) {
  console.log(v === null);
}

// Rethrowing preserves identity: `throw e` re-raises the same value, it does
// not wrap or copy it.
var thrown = { tag: "same" };
try {
  try {
    throw thrown;
  } catch (v) {
    throw v;
  }
} catch (v) {
  console.log(v === thrown);
}

// A closure made in the catch body captures the parameter and outlives it.
function capture() {
  try {
    throw "captured";
  } catch (v) {
    return function () {
      return v;
    };
  }
}
console.log(capture()());
