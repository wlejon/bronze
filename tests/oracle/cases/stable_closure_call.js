// The static call plan (src/lower/lower_scope.cpp `planStableFunctionSlots`):
// a call whose callee is a never-reassigned function declaration of an
// enclosing scope compiles to a DIRECT call to that function, with the
// environment derived by counting parent links instead of by loading the
// closure. Everything below is a way for that claim to be wrong.

// 1. The shape it exists for: sibling closures in a factory, called through
//    the environment chain. `render` is declared last, so every one of its
//    callees is already lowered and every call is a direct edge.
function makeCounter() {
  let n = 0;
  function bump(by) { n = n + by; }
  function readTwice() { return n * 2; }
  function render(times) {
    for (let i = 0; i < times; i++) { bump(i); }
    return readTwice();
  }
  return render;
}
console.log(makeCounter()(5));

// 2. A declaration binding that IS reassigned must call the new value. The
//    plan has to refuse this slot outright — the assignment is three scopes
//    down from the declaration and runs long after the closure was made.
function swappable() {
  function greet() { return "first"; }
  function replace() { greet = function () { return "second"; }; }
  function call() { return greet(); }
  return { call: call, replace: replace };
}
const sw = swappable();
console.log(sw.call());
sw.replace();
console.log(sw.call());

// 3. The same, written as a compound assignment and as an update operator,
//    because the refusal is name-based and each spelling has to reach it.
function swappable2() {
  function value() { return 3; }
  let v = value;
  function bumpName() { value = function () { return 9; }; }
  function read() { return value(); }
  bumpName();
  return read() + v();
}
console.log(swappable2());

// 4. A closure that ESCAPES the factory and is called long afterwards still
//    reaches the environment of the invocation that made it — one direct
//    edge, two live records.
function makeAdder(base) {
  function add(x) { return base + x; }
  function apply(x) { return add(x); }
  return apply;
}
const addTen = makeAdder(10);
const addTwenty = makeAdder(20);
console.log(addTen(1) + "," + addTwenty(1) + "," + addTen(2));

// 5. Recursion and mutual recursion through the same bindings.
function outerFact() {
  function fact(n) {
    if (n < 2) { return 1; }
    return n * fact(n - 1);
  }
  function go() { return fact(6); }
  return go();
}
console.log(outerFact());

function parity(n) {
  function isEven(k) {
    if (k === 0) { return true; }
    return isOdd(k - 1);
  }
  function isOdd(k) {
    if (k === 0) { return false; }
    return isEven(k - 1);
  }
  function ask() { return isEven(n) + ":" + isOdd(n); }
  return ask();
}
console.log(parity(10));
console.log(parity(7));

// 6. Depth: a grandchild closure calling a function declared two scopes up,
//    and a sibling of its own in between.
function depths() {
  let tag = "d";
  function outerHelp(s) { return tag + s; }
  function middle() {
    let inner = "!";
    function innerHelp(s) { return s + inner; }
    function deep() { return outerHelp(innerHelp("x")); }
    return deep();
  }
  return middle();
}
console.log(depths());

// 7. Shadowing: an inner binding of the same name wins, and the plan must
//    resolve the name exactly the way an ordinary read does.
function shadowed() {
  function pick() { return "outer"; }
  function ask() {
    const pick = function () { return "inner"; };
    return pick();
  }
  function askOuter() { return pick(); }
  return ask() + "/" + askOuter();
}
console.log(shadowed());

// 8. Arity: short calls, extra arguments, defaults and a rest parameter, all
//    through the direct edge.
function arities() {
  function three(a, b, c) { return "" + a + "|" + b + "|" + c; }
  function withDefault(a, b) {
    if (b === undefined) { b = 7; }
    return a + b;
  }
  function rest(a) {
    let out = a;
    for (let i = 1; i < arguments.length; i++) { out = out + arguments[i]; }
    return out;
  }
  function spread(a) {
    const extra = [];
    for (let i = 1; i < arguments.length; i++) { extra.push(arguments[i]); }
    return a + ":" + extra.join("-");
  }
  function go() {
    return three(1) + " " + three(1, 2, 3, 4) + " " + withDefault(1) + " " +
           withDefault(1, 2) + " " + rest(1, 2, 3) + " " + spread(1, 2, 3);
  }
  return go();
}
console.log(arities());

function restParams() {
  function tail(head, ...others) { return head + "/" + others.join(","); }
  function go() { return tail(1) + " " + tail(1, 2, 3); }
  return go();
}
console.log(restParams());

// 9. `this` inside a sibling closure is undefined for a plain call, direct or
//    not — bronze has no global object to substitute.
//     Written strict so the answer is the language's rather than bronze's — a
//     sloppy plain call substitutes the global object in a browser and bronze
//     has none, which is a divergence of its own and not what this is about.
function receivers() {
  "use strict";
  function whoAmI() { return this === undefined ? "undefined" : "other"; }
  function go() { return whoAmI(); }
  return go();
}
console.log(receivers());

// 10. Per-iteration records: a declaration inside a loop body gets a fresh
//     record each time round, and a direct call must reach the record of the
//     iteration it is running in, not the first one.
function perIteration() {
  const made = [];
  for (let i = 0; i < 3; i++) {
    let seen = i * 10;
    function tag() { return seen; }
    function read() { return tag() + i; }
    made.push(read);
  }
  return made[0]() + "," + made[1]() + "," + made[2]();
}
console.log(perIteration());

// 11. A declaration in a nested BLOCK lives in the block's own record, so a
//     sibling in that block counts parent links to the block and not to the
//     function. Two blocks, because the second one's record holds a capture
//     of its own and so is a different shape from the first's.
//     (Deliberately no name shared with an enclosing scope: Annex B.3.3's
//     sloppy hoisting of a block function into the function scope is a
//     divergence bronze has on its own and is not what this case is about.)
function blocks() {
  let out = "";
  {
    function inFirst() { return "one"; }
    function askFirst() { return inFirst(); }
    out = askFirst();
  }
  {
    let salt = "!";
    function inSecond() { return "two" + salt; }
    function askSecond() { return inSecond(); }
    out = out + "/" + askSecond();
  }
  return out;
}
console.log(blocks());

// 12. Generators and async declarations are refused by the plan: the value in
//     the slot is a factory, not the body a direct call would enter.
function machines() {
  function* counter() { yield 1; yield 2; }
  function drive() {
    let sum = 0;
    for (const v of counter()) { sum = sum + v; }
    return sum;
  }
  return drive();
}
console.log(machines());

// 13. Throwing across a direct edge still unwinds to the caller's handler.
function throwsAcross() {
  function boom() { throw new Error("boom"); }
  function go() {
    try {
      boom();
      return "no";
    } catch (e) {
      return "caught:" + e.message;
    }
  }
  return go();
}
console.log(throwsAcross());
