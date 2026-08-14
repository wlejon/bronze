// BLOCKED: `arguments.callee` (ECMA-262 10.2.11 step 6, 10.2.4, 10.4.4).
//
// bronze diagnoses it by name in BOTH modes, so every line here is a hard error
// today rather than a wrong answer. What is missing is one thing: the function
// object of the running invocation.
//
// A compiled body is entered through its code pointer with
// `(env, this, arguments, ...params)`. The FunctionHeader that wraps that
// pointer is not among them, and a DIRECT typed call does not construct one at
// all — that is the point of a direct call. So the sloppy answer, which 10.4.4
// says is that function object, needs a new channel on the calling convention
// that every call in the program would pay for, direct calls included.
//
// The strict answer is a TypeError from the poison pill 10.2.4 defines, and
// bronze could give it exactly — the accessor pair is already installed on the
// arguments object (rt_spread.cpp), and only its halves would change. What it
// cannot do is tell the two modes apart down here: strictness is a
// per-INSTRUCTION fact in lowering (`strictFlag()`), and no `il::Function`
// carries it, so `bronze_arguments_object` has no way to be told which object
// to build. Giving the strict case its TypeError is therefore the cheaper half
// and would land first: it needs a strict bit on the IL function and a
// parameter on the ABI helper, and nothing about the calling convention.
//
// The expectation below is the language's, for both halves:
//
// 1. In sloppy code `arguments.callee` is the function currently executing, so
//    it is `===` the binding that names it — and it is the same for a function
//    EXPRESSION, which is the case the feature was ever really used for.
// 2. It survives being handed around: `callee` is a data property of the
//    arguments object (10.2.12 for a mapped one), not a magic read.
// 3. In strict code the property is an accessor whose get and set halves are
//    both %ThrowTypeError%, so merely READING it throws — the "poison pill".
// 4. An arrow has no `arguments` of its own, so `arguments.callee` inside one
//    is the ENCLOSING function, not the arrow.

function named() {
  return arguments.callee;
}
console.log(named() === named);

const expr = function () {
  return arguments.callee;
};
console.log(expr() === expr);

function recurse(n) {
  if (n === 0) return 0;
  return n + arguments.callee(n - 1);
}
console.log(recurse(4));

function strictCallee() {
  "use strict";
  try {
    return arguments.callee;
  } catch (e) {
    return e instanceof TypeError ? e.name : "wrong error";
  }
}
console.log(strictCallee());

function outerCallee() {
  const inner = () => arguments.callee;
  return inner() === outerCallee;
}
console.log(outerCallee());
