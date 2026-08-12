// A name nothing declares, EVALUATED.
//
// From ECMA-262 6.2.5.5 (GetValue), 13.15.2 (assignment) and 20.5.7
// (NativeError objects):
//
// 1. GetValue step 2: "If IsUnresolvableReference(V) is true, throw a
//    ReferenceError exception." The throw happens where the reference is
//    evaluated, so a function that names `document` and is never CALLED
//    raises nothing — its body is not evaluated. That is why the two
//    definitions below can be compiled and the program run normally.
// 2. The error is an ordinary ReferenceError instance: catchable, with
//    `name` "ReferenceError" from its prototype (20.5.7.3.2) and a `message`
//    naming the identifier, and `instanceof` true for both ReferenceError
//    and Error, whose prototype is on its chain (20.5.7.1).
// 3. 13.15.2 (simple assignment) evaluates the right side BEFORE PutValue, so
//    `missing = sideEffect()` runs the call and then throws; 13.15.4 (compound
//    assignment) begins with GetValue of the target, so `missing += 1` throws
//    before its right side runs at all. A module is strict code (16.2.1.6.4),
//    so PutValue on an unresolvable reference throws rather than creating a
//    global.
// 4. An unresolvable name inside an argument list throws while the arguments
//    are being evaluated, so the call never happens.
//
// The compile-time warning bronze emits for each of these names goes to
// stderr, which is why this file's pinned stdout is unaffected by it.

function createElementNS(name) {
  return document.createElementNS("http://www.w3.org/2000/svg", name);
}

function neverCalled() {
  return window.innerWidth + fetch("/x");
}

console.log("compiled and running");
console.log(typeof createElementNS, typeof neverCalled);

try {
  createElementNS("div");
  console.log("no throw");
} catch (e) {
  console.log(e.name);
  console.log(e.message);
  console.log(e instanceof ReferenceError, e instanceof Error);
}

// The member expression `typeof` does NOT exempt: 13.5.3's step 1 is about an
// unresolvable reference, and `__MISSING__.version` evaluates `__MISSING__`.
try {
  console.log(typeof __MISSING__.version);
} catch (e) {
  console.log(e.message);
}

let sideEffects = 0;
function bump() {
  sideEffects = sideEffects + 1;
  return 1;
}

try {
  missingTarget = bump();
} catch (e) {
  console.log(e.message);
}
console.log("right side ran:", sideEffects);

try {
  missingTarget += bump();
} catch (e) {
  console.log(e.message);
}
console.log("right side ran:", sideEffects);

try {
  console.log("unreachable", missingArgument);
} catch (e) {
  console.log(e.message);
}

// The throw leaves the whole program intact: nothing above was skipped and
// everything below still runs.
console.log("still here");
