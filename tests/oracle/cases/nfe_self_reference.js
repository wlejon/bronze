// A named function EXPRESSION can refer to itself by its own name
// (ECMA-262 15.2.5 InstantiateOrdinaryFunctionExpression).
//
// Step 2 onwards: the name is bound in a declarative environment that is
// created around the function and encloses its scope, and nothing else. Three
// consequences, all pinned below.
//
// It is a BINDING, not a property, so the recursive call works even when the
// expression is anonymous to the outside world and even after whatever variable
// held it is reassigned — which is the whole reason the form exists.
//
// It is IMMUTABLE (step 3 is CreateImmutableBinding with strict = true), so an
// assignment to it inside the body goes to 9.1.1.1.5 SetMutableBinding step 4:
// a TypeError in strict code, a quiet return in sloppy code. Not an error at
// compile time, because whether the write ever runs is a runtime question, and
// not a silent success either.
//
// It is INVISIBLE outside: the environment is created for this one function, so
// the name is not in scope after the expression, and anything of the same name
// declared INSIDE — a parameter, a `var`, a `let` — is in a nested environment
// and shadows it outright.
//
// Function DECLARATIONS are untouched by any of this: their name is an ordinary
// mutable binding of the enclosing scope, which is why the last two lines can
// write to one.

console.log(
  (function fact(n) {
    return n <= 1 ? 1 : n * fact(n - 1);
  })(5)
);

console.log(
  (function f() {
    return typeof f;
  })()
);

// The binding survives the variable being reassigned: `self` is the function,
// `holder` is not, and the recursion goes through the binding.
let holder = function countdown(n) {
  return n === 0 ? "done" : countdown(n - 1);
};
const kept = holder;
holder = "no longer a function";
console.log(kept(3), typeof holder);

// Sloppy: the write is a quiet no-op and the binding still names the function.
const sloppyWrite = function self() {
  self = 99;
  return typeof self;
};
console.log(sloppyWrite());

// Strict: the same write is a TypeError, plain and compound alike.
const strictWrite = function self() {
  "use strict";
  try {
    self = 1;
    return "no throw";
  } catch (e) {
    return e instanceof TypeError;
  }
};
console.log(strictWrite());

const strictCompound = function self() {
  "use strict";
  try {
    self += 1;
    return "no throw";
  } catch (e) {
    return e.name;
  }
};
console.log(strictCompound());

// A parameter of the same name shadows the binding, and so does an inner `var`.
console.log(
  (function s(s) {
    return s;
  })("param")
);
console.log(
  (function s() {
    var s = "inner var";
    return s;
  })()
);
console.log(
  (function s() {
    let s = "inner let";
    return s;
  })()
);

// The name does not leak: neither of these is in scope out here.
console.log(typeof fact, typeof countdown);

// The binding roots the closure, so an escaping one still recurses long after
// the expression that created it returned — including through a GC.
function make() {
  return function loop(n) {
    return n === 0 ? [] : loop(n - 1).concat([n]);
  };
}
const escaped = make();
console.log(escaped(4).join(","));

const nested = (function outer() {
  return function () {
    return typeof outer;
  };
})();
console.log(nested());

// A declaration's name is an ordinary mutable binding of the enclosing scope.
function decl() {
  return typeof decl;
}
console.log(decl());
{
  function inner() {
    return "inner-decl";
  }
  console.log(inner());
}
