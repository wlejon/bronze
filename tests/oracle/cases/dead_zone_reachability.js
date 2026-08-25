// Whether a temporal dead zone can be REACHED, which is what decides whether
// the check for it has to be emitted (src/ast/queries.h,
// `getDefinitelyAssignedLexicalNames`).
//
// The dead zone of a `let` is a stretch of evaluation, not a stretch of source
// text: a closure over the binding is entered only by someone CALLING it, and
// calling is user code. So if every statement from the top of a scope down to
// a declaration runs no user code — another literal-initialized declaration,
// or a hoisted `function` — then nothing can have called anything, and every
// read of that binding anywhere in the program is after its initializer.
//
// Each pair below is the same program twice: once in that shape, and once with
// exactly one statement changed so that user code CAN run before the
// initializer. The second of each pair is the one the check exists for, and the
// day the analysis widens far enough to swallow it this file stops matching
// node.

// 1. The shape itself: a closure over a literal-initialized `let`, called from
//    below the declaration. No dead zone is reachable and no read can throw.
function unreachable() {
  function peek() { return flag; }
  let flag = "one";
  return peek();
}

// 2. The same closure, but the declaration ABOVE it calls out — and that call
//    is a moment at which `peek` runs, with `flag` still uninitialized.
function reachableThroughAnEarlierCall() {
  function peek() { return flag; }
  function fire() { return peek(); }
  let started = tryIt(fire);
  let flag = "two";
  return `${started}/${flag}`;
}

// 3. A binding read by its OWN initializer's call, which is the narrowest
//    version of the same hazard: nothing precedes the declaration at all.
function reachableThroughItsOwnInitializer() {
  function peek() { return late; }
  let late = tryIt(peek);
  return late;
}

// 4. `const`, same two shapes — the binding form the state closures in real
//    code are written with.
function constUnreachable() {
  function read() { return k; }
  const k = 40;
  return read() + 2;
}

function constReachable() {
  function read() { return k; }
  function fire() { return read(); }
  const first = tryIt(fire);
  const k = 40;
  return `${first}/${k}`;
}

// 5. A block scope, where the same rule applies to the block's own statement
//    list rather than the function's.
function blockScopes() {
  const out = [];
  {
    function inner() { return blocked; }
    let blocked = "inner-ok";
    out.push(inner());
  }
  {
    function inner2() { return alsoBlocked; }
    function fire() { return inner2(); }
    out.push(tryIt(fire));
    let alsoBlocked = "never";
  }
  return out.join(",");
}

// 6. A `for` head binding, which is copied per iteration: every copy is written
//    before the body runs, so a closure over it never sees a dead zone — but
//    the head's own initializer can still call out.
function loopBindings() {
  const seen = [];
  for (let i = 0; i < 2; i++) {
    seen.push((() => i)());
  }
  return seen.join(",");
}

// 7. The declaration order that makes the prefix stop: a `let` whose
//    initializer is a call sits above a literal-initialized one, and BOTH keep
//    their dead zones because the call is a moment when anything can run.
function prefixStops() {
  function peekSecond() { return second; }
  function fire() { return peekSecond(); }
  let first = tryIt(fire);
  let second = "seven";
  return `${first}/${second}`;
}

// The harness: run `fn` and report what it did, so a throw is DATA rather than
// the end of the program.
function tryIt(fn) {
  try {
    return `ok:${fn()}`;
  } catch (e) {
    return `${e.name}`;
  }
}

console.log(unreachable());
console.log(reachableThroughAnEarlierCall());
console.log(tryIt(reachableThroughItsOwnInitializer));
console.log(constUnreachable());
console.log(constReachable());
console.log(blockScopes());
console.log(loopBindings());
console.log(prefixStops());

// And the one an unreachable dead zone must not turn into: a binding read
// before its initializer through a call the analysis CAN see, at the top level
// of the program rather than inside a function.
function topPeek() { return topLet; }
function topFire() { return topPeek(); }
const topFirst = tryIt(topFire);
let topLet = "top";
console.log(`${topFirst}/${topLet}`);
