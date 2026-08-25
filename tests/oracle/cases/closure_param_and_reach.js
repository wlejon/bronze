// The two stage E4 proofs, pinned from the side each of them REFUSES.
//
// 1. The closure parameter proof (src/lower/lower_scope.cpp,
//    `planClosureParamNumbers`): a nested `function f(x)` every one of whose
//    call sites this compilation can enumerate gets an f64 parameter slot,
//    because the join over those sites is then a fact about every call. Cases
//    1-10 are the doors a function value can leave through and the argument
//    shapes that break the one-argument-per-parameter correspondence. Each of
//    them must keep the boxed convention, so `typeof x` still answers `string`
//    for a string and `arguments[0]` still holds what the caller passed. The
//    day the proof widens far enough to swallow one of them, this file stops
//    matching node.
//
// 2. The widened dead-zone reach analysis (src/ast/queries_declaration.cpp,
//    `getDefinitelyAssignedLexicalNames`): a statement runs no code that can
//    READ this scope's bindings unless it can reach a closure over this
//    scope's record, so a call to something else carries the scan on. Cases
//    11-18 are the ways such a closure IS reachable — handed to a call, built
//    as an IIFE, built as a callback, built by a parameter default, built as
//    an object literal's method — every one of which must still raise the
//    ReferenceError 9.1.1.1.6 puts there.
function tryIt(fn) {
  try { return `ok:${fn()}`; } catch (e) { return e.name; }
}
function outer(v) { return v; }

// --- closure parameter proof --------------------------------------------
// 1. escaping closure: `g` is returned, so nothing enumerates its callers.
function escapes() {
  function g(x) { return typeof x + ':' + x; }
  return g;
}
console.log(escapes()('s'), escapes()(2));

// 2. reached through .call
function throughCall() {
  function g(x) { return typeof x + ':' + x; }
  return g.call(null, 's') + '|' + g(1);
}
console.log(throughCall());

// 3. a short call: position 1 is undefined at one site
function shortCall() {
  function g(x, y) { return `${typeof x}/${typeof y}`; }
  return g(1) + '|' + g(2, 3);
}
console.log(shortCall());

// 4. the name is reassigned
function rebound() {
  function g(x) { return typeof x + ':' + x; }
  g = function (x) { return 'other:' + x; };
  return g('s');
}
console.log(rebound());

// 5. spread at a site
function spread() {
  function g(x, y) { return `${typeof x}/${typeof y}`; }
  const a = ['s', 2];
  return g(...a) + '|' + g(1, 2);
}
console.log(spread());

// 6. one site passes a string
function mixed() {
  function g(x) { return typeof x + ':' + x; }
  return g(1) + '|' + g('s');
}
console.log(mixed());

// 7. an optional call is still a call
function optional() {
  function g(x) { return typeof x + ':' + x; }
  return g?.(1) + '|' + g(2);
}
console.log(optional());

// 8. `arguments` must still see what the CALLER passed, not the f64 slot
function argsObject() {
  function g(x) { return `${typeof x}:${x}/${typeof arguments[0]}:${arguments[0]}/${arguments.length}`; }
  return g(1) + '|' + g(2, 3);
}
console.log(argsObject());

// 9. proven, and the proof must not change the answer
function proven() {
  function g(x, y) { return x * 2 + y; }
  let t = 0;
  for (let i = 0; i < 4; i++) t += g(i, i & 1);
  return t;
}
console.log(proven());

// 10. a property read of the function value is an escape
function readsName() {
  function g(x) { return typeof x + ':' + x; }
  return g.length + '|' + g('s');
}
console.log(readsName());

// --- widened dead-zone reach --------------------------------------------
// 11. a call that cannot reach this scope's closures: `b` is proven, and the
//     answer must be the same either way.
function callThatCannotReach() {
  const a = outer(7);
  let b = 1;
  function peek() { return b; }
  return `${a}/${peek()}`;
}
console.log(callThatCannotReach());

// 12. the same call, handed this scope's closure
function callThatCanReach() {
  function peek() { return b; }
  const a = tryIt(peek);
  let b = 1;
  return `${a}/${b}`;
}
console.log(callThatCanReach());

// 13. an IIFE in the prefix
function iife() {
  const a = tryIt(function () { return b; });
  let b = 1;
  return `${a}/${b}`;
}
console.log(iife());

// 14. an arrow handed to a call in the prefix
function arrowArg() {
  const a = [1].map(() => tryIt(() => b))[0];
  let b = 1;
  return `${a}/${b}`;
}
console.log(arrowArg());

// 15. an object literal's method in the prefix
function objectMethod() {
  const o = { m() { return b; } };
  const a = tryIt(o.m);
  let b = 1;
  return `${a}/${b}`;
}
console.log(objectMethod());

// 16. a closure built in the prefix and entered there
function readsLater() {
  const a = tryIt(function () { return b; });
  let b = 1;
  return `${a}/${b}`;
}
console.log(readsLater());

// 17. a whole loop in the prefix, which is the shape typed_array_crunch needs
//     and the one stage E3's rule could not pass
function loopInThePrefix() {
  const xs = [];
  for (let i = 0; i < 3; i++) xs.push(outer(i));
  const total = 10;
  let seen = 0;
  function count() { return seen + total; }
  seen = xs.length;
  return count();
}
console.log(loopInThePrefix());

// 18. the loop body reaching a closure of this scope
function loopThatReaches() {
  function peek() { return late; }
  const seen = [];
  for (let i = 0; i < 2; i++) seen.push(tryIt(peek));
  let late = 'x';
  return seen.join(',') + '/' + late;
}
console.log(loopThatReaches());
